#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

volatile sig_atomic_t stopping = 0;
static void stop_signal(int signal_number) {
    (void)signal_number;
    stopping = 1;
}
void install_signals(void) {
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}
int set_fd_flags(int fd, bool nonblocking, CaError *error) {
    int descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 || fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
        return ca_errno(error, "fcntl CLOEXEC");
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0)
        return ca_errno(error, "fcntl NONBLOCK");
    return 0;
}
static int setup_socket(int fd, CaError *error) {
    if (set_fd_flags(fd, true, error) < 0)
        return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return 0;
}
int listen_tcp(const char *host, int port, CaError *error) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return ca_errno(error, "socket");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        close(fd);
        return ca_error(error, CA_ERROR, "监听地址必须为 IPv4 地址");
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 16) < 0 ||
        setup_socket(fd, error) < 0) {
        if (!error->code)
            ca_errno(error, "bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}
int connect_tcp(const char *host, int port, CaError *error) {
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int status = getaddrinfo(host, service, &hints, &addresses);
    if (status)
        return ca_error(error, CA_ERROR, "%s", gai_strerror(status));
    int result = -1, saved = ECONNREFUSED;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 && setup_socket(fd, error) == 0) {
            result = fd;
            break;
        }
        saved = errno;
        close(fd);
    }
    freeaddrinfo(addresses);
    if (result < 0 && !error->code) {
        errno = saved;
        ca_errno(error, "connect");
    }
    return result;
}
int socket_port(int fd, CaError *error) {
    struct sockaddr_in address = {0};
    socklen_t n = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &n) < 0)
        return ca_errno(error, "getsockname");
    return ntohs(address.sin_port);
}
static int wait_ready(int fd, short events, int64_t deadline, CaError *error) {
    for (;;) {
        if (stopping)
            return ca_error(error, CA_CANCELLED, "进程停止");
        int64_t left = deadline - monotonic_ms();
        if (left <= 0)
            return ca_error(error, CA_TIMEOUT, "通信超时");
        struct pollfd descriptor = {fd, events, 0};
        int ready = poll(&descriptor, 1, left > 200 ? 200 : (int)left);
        if (ready > 0) {
            if (descriptor.revents & POLLNVAL)
                return ca_error(error, CA_ERROR, "socket 已关闭");
            return 0;
        }
        if (ready < 0 && errno != EINTR)
            return ca_errno(error, "poll");
    }
}
static int receive_all(int fd, void *bytes, size_t count, int64_t deadline, CaError *error) {
    size_t offset = 0;
    while (offset < count) {
        if (wait_ready(fd, POLLIN, deadline, error) < 0)
            return -1;
        ssize_t n = recv(fd, (char *)bytes + offset, count - offset, 0);
        if (n == 0) {
            if (offset)
                return ca_error(error, CA_ERROR, "不完整 TCP 消息");
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return ca_errno(error, "recv");
        }
        offset += (size_t)n;
    }
    return 1;
}
int send_frame(int fd, const JVal *message, int timeout_ms, CaError *error) {
    size_t n = 0;
    char *body = json_dump_value(message, false, &n);
    if (!body)
        return ca_error(error, CA_NOMEM, "JSON 序列化失败");
    if (!n || n > CA_FRAME_LIMIT) {
        free(body);
        return ca_error(error, CA_ERROR, "消息超过 1 MiB");
    }
    uint32_t header = htonl((uint32_t)n);
    Buffer frame = {0};
    int code = -1;
    if (buf_append(&frame, &header, sizeof(header), error) < 0 ||
        buf_append(&frame, body, n, error) < 0)
        goto done;
    int64_t deadline = monotonic_ms() + timeout_ms;
    size_t offset = 0;
    while (offset < frame.len) {
        if (wait_ready(fd, POLLOUT, deadline, error) < 0)
            goto done;
        ssize_t sent = send(fd, frame.data + offset, frame.len - offset, 0);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (sent <= 0) {
            ca_error(error, CA_CANCELLED, "客户端已断开");
            goto done;
        }
        offset += (size_t)sent;
    }
    code = 0;
done:
    free(body);
    buf_free(&frame);
    return code;
}
int receive_frame(int fd, Json **message, int timeout_ms, CaError *error) {
    *message = NULL;
    uint32_t header = 0;
    int64_t deadline = monotonic_ms() + timeout_ms;
    int received = receive_all(fd, &header, sizeof(header), deadline, error);
    if (received <= 0)
        return received;
    size_t n = ntohl(header);
    if (!n || n > CA_FRAME_LIMIT)
        return ca_error(error, CA_ERROR, "非法消息长度");
    char *bytes = malloc(n);
    if (!bytes)
        return ca_error(error, CA_NOMEM, "内存不足");
    received = receive_all(fd, bytes, n, deadline, error);
    if (received != 1) {
        free(bytes);
        if (!received)
            ca_error(error, CA_ERROR, "消息正文截断");
        return -1;
    }
    Json *value = json_parse(bytes, n);
    free(bytes);
    if (!value || !json_is_object(JV(value))) {
        json_free(value);
        return ca_error(error, CA_ERROR, "消息必须为合法 JSON 对象");
    }
    *message = value;
    return 1;
}
bool peer_closed(int fd) {
    char byte;
    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}
int inbox_receive(FrameInbox *inbox, int fd, CaEmit consume, void *ctx, CaError *error) {
    for (;;) {
        char bytes[8192];
        ssize_t n = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        if (n < 0)
            return ca_errno(error, "recv");
        if (!n) {
            if (inbox->bytes.len)
                return ca_error(error, CA_ERROR, "消息正文截断");
            return 0;
        }
        if (buf_append(&inbox->bytes, bytes, (size_t)n, error) < 0)
            return -1;
        while (inbox->bytes.len >= 4) {
            uint32_t size;
            memcpy(&size, inbox->bytes.data, 4);
            size = ntohl(size);
            if (!size || size > CA_FRAME_LIMIT)
                return ca_error(error, CA_ERROR, "非法消息长度");
            if (inbox->bytes.len < 4 + (size_t)size)
                break;
            Json *message = json_parse(inbox->bytes.data + 4, size);
            if (!message || !json_is_object(JV(message))) {
                json_free(message);
                return ca_error(error, CA_ERROR, "消息必须为合法 JSON 对象");
            }
            buf_consume(&inbox->bytes, 4 + (size_t)size);
            int code = consume(ctx, JV(message), error);
            json_free(message);
            if (code < 0)
                return -1;
        }
    }
}
