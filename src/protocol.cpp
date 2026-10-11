#include "common.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace ca {
volatile std::sig_atomic_t stopping = 0;
Fd::~Fd() { if (fd_ >= 0) ::close(fd_); }
Fd::Fd(Fd&& other) noexcept : fd_(other.release()) {}
Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = other.release(); }
    return *this;
}
int Fd::release() { int value = fd_; fd_ = -1; return value; }
static void fail(const std::string& action) {
    throw std::runtime_error(action + ": " + std::strerror(errno));
}
void set_fd_flags(int fd, bool nonblocking) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) fail("fcntl CLOEXEC");
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0)
        fail("fcntl NONBLOCK");
}
static void setup_socket(int fd) {
    set_fd_flags(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}
Fd listen_tcp(const std::string& host, int port) {
    Fd socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (socket.get() < 0) fail("socket");
    int one = 1;
    setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("监听地址必须为 IPv4 地址");
    if (::bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) fail("bind");
    if (::listen(socket.get(), 16) < 0) fail("listen");
    setup_socket(socket.get());
    return socket;
}
Fd connect_tcp(const std::string& host, int port) {
    addrinfo hints{}, *raw = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int code = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &raw);
    if (code) throw std::runtime_error(gai_strerror(code));
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
    for (auto* a = raw; a; a = a->ai_next) {
        Fd socket(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (socket.get() >= 0 && ::connect(socket.get(), a->ai_addr, a->ai_addrlen) == 0) {
            setup_socket(socket.get());
            return socket;
        }
    }
    fail("connect");
    return Fd();
}
int socket_port(int fd) {
    sockaddr_in address{};
    socklen_t size = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) < 0) fail("getsockname");
    return ntohs(address.sin_port);
}
static void wait_ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        if (stopping) throw Cancelled("进程停止");
        int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
        if (left <= 0) throw std::runtime_error("通信超时");
        pollfd descriptor{fd, events, 0};
        int ready = poll(&descriptor, 1, std::min(left, 200));
        if (ready > 0) {
            if (descriptor.revents & POLLNVAL) throw std::runtime_error("socket 已关闭");
            return;
        }
        if (ready < 0 && errno != EINTR) fail("poll");
    }
}
static bool receive_all(int fd, char* bytes, std::size_t count, Clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < count) {
        wait_ready(fd, POLLIN, deadline);
        ssize_t got = recv(fd, bytes + offset, count - offset, 0);
        if (got == 0) {
            if (offset) throw std::runtime_error("不完整 TCP 消息");
            return false;
        }
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fail("recv");
        }
        offset += static_cast<std::size_t>(got);
    }
    return true;
}
void send_frame(int fd, const Json& message, int timeout_ms) {
    std::string body = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (body.size() > frame_limit) throw std::runtime_error("消息超过 1 MiB");
    std::uint32_t size = htonl(static_cast<std::uint32_t>(body.size()));
    std::string frame(reinterpret_cast<char*>(&size), sizeof(size));
    frame += body;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t offset = 0;
    while (offset < frame.size()) {
        wait_ready(fd, POLLOUT, deadline);
        ssize_t sent = send(fd, frame.data() + offset, frame.size() - offset, 0);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw Cancelled("客户端已断开");
        }
        if (!sent) throw Cancelled("客户端已断开");
        offset += static_cast<std::size_t>(sent);
    }
}
std::optional<Json> receive_frame(int fd, int timeout_ms) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::uint32_t header = 0;
    if (!receive_all(fd, reinterpret_cast<char*>(&header), sizeof(header), deadline)) return {};
    auto size = ntohl(header);
    if (!size || size > frame_limit) throw std::runtime_error("非法消息长度");
    std::string body(size, '\0');
    if (!receive_all(fd, body.data(), size, deadline)) throw std::runtime_error("消息正文截断");
    Json message = Json::parse(body);
    if (!message.is_object()) throw std::runtime_error("消息必须为 JSON 对象");
    return message;
}
bool peer_closed(int fd) {
    char byte;
    ssize_t value = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return value == 0 || (value < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}
bool FrameInbox::receive(int fd, const Emit& consume) {
    for (;;) {
        char bytes[8192];
        ssize_t got = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (got < 0) fail("recv");
        if (!got) {
            if (!buffer_.empty()) throw std::runtime_error("消息正文截断");
            return false;
        }
        buffer_.append(bytes, static_cast<std::size_t>(got));
        while (buffer_.size() >= 4) {
            std::uint32_t size;
            std::memcpy(&size, buffer_.data(), 4);
            size = ntohl(size);
            if (!size || size > frame_limit) throw std::runtime_error("非法消息长度");
            if (buffer_.size() < 4 + size) break;
            auto message = Json::parse(buffer_.substr(4, size));
            if (!message.is_object()) throw std::runtime_error("消息必须为 JSON 对象");
            buffer_.erase(0, 4 + size);
            consume(message);
        }
    }
}
void install_signals() {
    struct sigaction action{};
    action.sa_handler = [](int) { stopping = 1; };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
}
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&seconds, &value);
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3)
           << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000 << 'Z';
    return output.str();
}
std::string utf8_prefix(std::string& pending, bool final) {
    std::size_t pos = 0;
    while (pos < pending.size()) {
        unsigned char head = static_cast<unsigned char>(pending[pos]);
        std::size_t width = head < 0x80 ? 1 : (head & 0xe0) == 0xc0 ? 2 : (head & 0xf0) == 0xe0 ? 3 : (head & 0xf8) == 0xf0 ? 4 : 1;
        if (pos + width > pending.size() && !final) break;
        pos += std::min(width, pending.size() - pos);
    }
    std::string ready = pending.substr(0, pos);
    pending.erase(0, pos);
    return ready;
}
void append_json(std::ofstream& file, const Json& item) {
    file << item.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
    file.flush();
    if (!file) throw std::runtime_error("写日志失败");
}
} // namespace ca
