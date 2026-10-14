#ifndef CA_TEST_SUPPORT_H
#define CA_TEST_SUPPORT_H
#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ftw.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static CaError error;
static pid_t test_owner, test_processes[128];
static size_t test_process_count;
static char test_root[128];
static bool test_success;
static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *walk) {
    (void)st;
    (void)type;
    (void)walk;
    return remove(path);
}
static void test_cleanup(void) {
    if (test_owner != getpid())
        return;
    for (size_t i = 0; i < test_process_count; ++i)
        kill(-test_processes[i], SIGTERM);
    int64_t deadline = monotonic_ms() + 2000;
    for (size_t i = 0; i < test_process_count; ++i) {
        pid_t pid = test_processes[i];
        while (waitpid(pid, NULL, WNOHANG) == 0 && monotonic_ms() < deadline)
            sleep_ms(10);
        kill(-pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    if (*test_root) {
        if (test_success)
            nftw(test_root, remove_entry, 32, FTW_DEPTH | FTW_PHYS);
        else
            fprintf(stderr, "test files: %s\n", test_root);
    }
}
static void test_fail(const char *message, const char *file, int line) {
    fprintf(stderr, "FAIL %s:%d: %s%s%s\n", file, line, message, error.code ? " | " : "",
            error.code ? error.message : "");
    if (test_owner != getpid())
        _exit(1);
    exit(1);
}
#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        if (!(condition))                                                                          \
            test_fail(message, __FILE__, __LINE__);                                                \
    } while (0)
#define OK(action)                                                                                 \
    do {                                                                                           \
        ca_clear_error(&error);                                                                    \
        if ((action) < 0)                                                                          \
            test_fail(#action, __FILE__, __LINE__);                                                \
    } while (0)
static void test_init(void) {
    test_owner = getpid();
    signal(SIGPIPE, SIG_IGN);
    atexit(test_cleanup);
    strcpy(test_root, "/tmp/c-agent-tests-XXXXXX");
    CHECK(mkdtemp(test_root), "create temporary directory");
}
static void register_process(pid_t pid) {
    CHECK(pid > 0 && test_process_count < 128, "register child");
    setpgid(pid, pid);
    test_processes[test_process_count++] = pid;
}
static inline void raw_send(int fd, const void *data, size_t length, size_t fragment) {
    size_t offset = 0;
    while (offset < length) {
        size_t count = length - offset;
        if (fragment && count > fragment)
            count = fragment;
        ssize_t n = send(fd, (const char *)data + offset, count, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd wait = {fd, POLLOUT, 0};
            poll(&wait, 1, 100);
            continue;
        }
        CHECK(n > 0, "send raw bytes");
        offset += (size_t)n;
    }
}
static inline Buffer pack(const JVal *value) {
    size_t len;
    char *body = json_dump_value(value, false, &len);
    CHECK(body, "serialize frame");
    Buffer out = {0};
    uint32_t header = htonl((uint32_t)len);
    OK(buf_append(&out, &header, 4, &error));
    OK(buf_append(&out, body, len, &error));
    free(body);
    return out;
}
static char *test_path(const char *relative) {
    char *path = path_join(test_root, relative, &error);
    CHECK(path, "join test path");
    return path;
}
static Buffer read_text(const char *path) {
    Buffer text = {0};
    OK(read_file_bytes(path, 8 * CA_FRAME_LIMIT, &text, &error));
    return text;
}
static inline Buffer fixture_source(void) {
    return read_text(AGENT_PROJECT_DIR "/tests/fixtures/sort.c");
}
#endif
