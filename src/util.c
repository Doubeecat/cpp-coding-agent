#include "common.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int ca_error(CaError *error, CaErrorCode code, const char *format, ...) {
    if (error) {
        error->code = code;
        error->system_errno = 0;
        va_list ap;
        va_start(ap, format);
        vsnprintf(error->message, sizeof(error->message), format, ap);
        va_end(ap);
    }
    return -1;
}
int ca_errno(CaError *error, const char *action) {
    int saved = errno;
    ca_error(error, CA_ERROR, "%s: %s", action, strerror(saved));
    if (error)
        error->system_errno = saved;
    return -1;
}
void ca_clear_error(CaError *error) {
    if (error)
        memset(error, 0, sizeof(*error));
}
void buf_free(Buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}
int buf_append(Buffer *buffer, const void *data, size_t n, CaError *error) {
    if (buffer->len == SIZE_MAX || n > SIZE_MAX - buffer->len - 1)
        return ca_error(error, CA_NOMEM, "缓冲区长度溢出");
    size_t needed = buffer->len + n + 1;
    if (needed > buffer->cap) {
        size_t cap = buffer->cap ? buffer->cap : 64;
        while (cap < needed) {
            if (cap > SIZE_MAX / 2) {
                cap = needed;
                break;
            }
            cap *= 2;
        }
        char *next = realloc(buffer->data, cap);
        if (!next)
            return ca_error(error, CA_NOMEM, "内存不足");
        buffer->data = next;
        buffer->cap = cap;
    }
    if (n)
        memcpy(buffer->data + buffer->len, data, n);
    buffer->len += n;
    buffer->data[buffer->len] = 0;
    return 0;
}
int buf_printf(Buffer *buffer, CaError *error, const char *format, ...) {
    va_list ap, copy;
    va_start(ap, format);
    va_copy(copy, ap);
    int size = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (size < 0) {
        va_end(ap);
        return ca_error(error, CA_ERROR, "格式化文本失败");
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        va_end(ap);
        return ca_error(error, CA_NOMEM, "内存不足");
    }
    vsnprintf(text, (size_t)size + 1, format, ap);
    va_end(ap);
    int code = buf_append(buffer, text, (size_t)size, error);
    free(text);
    return code;
}
void buf_consume(Buffer *buffer, size_t n) {
    if (n > buffer->len)
        n = buffer->len;
    if (buffer->data) {
        memmove(buffer->data, buffer->data + n, buffer->len - n);
        buffer->len -= n;
        buffer->data[buffer->len] = 0;
    }
}
void buf_clear(Buffer *buffer) {
    buffer->len = 0;
    if (buffer->data)
        buffer->data[0] = 0;
}
const char *buf_text(const Buffer *buffer) {
    return buffer->data ? buffer->data : "";
}
char *ca_strdup(const char *s, CaError *error) {
    char *p = strdup(s);
    if (!p)
        ca_error(error, CA_NOMEM, "内存不足");
    return p;
}
void close_fd(int *fd) {
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}
int64_t monotonic_ms(void) {
    struct timespec t = {0};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) < 0 && errno == EINTR && !stopping) {
    }
}
void timestamp(char out[40]) {
    struct timespec t = {0};
    struct tm value;
    char base[24];
    clock_gettime(CLOCK_REALTIME, &t);
    gmtime_r(&t.tv_sec, &value);
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &value);
    snprintf(out, 40, "%s.%03ldZ", base, t.tv_nsec / 1000000);
}
size_t utf8_prefix_length(const char *s, size_t n, bool final) {
    size_t pos = 0;
    while (pos < n) {
        unsigned char head = (unsigned char)s[pos];
        size_t width = head < 0x80             ? 1
                       : (head & 0xe0) == 0xc0 ? 2
                       : (head & 0xf0) == 0xe0 ? 3
                       : (head & 0xf8) == 0xf0 ? 4
                                               : 1;
        if (width > n - pos) {
            if (!final)
                break;
            width = n - pos;
        }
        pos += width;
    }
    return pos;
}
int append_json(FILE *file, const JVal *item, CaError *error) {
    size_t size = 0;
    char *text = json_dump_value(item, false, &size);
    if (!text)
        return ca_error(error, CA_NOMEM, "JSON 序列化失败");
    bool ok = fwrite(text, 1, size, file) == size && fputc('\n', file) != EOF && fflush(file) == 0;
    free(text);
    return ok ? 0 : ca_errno(error, "写日志失败");
}
char *path_join(const char *a, const char *buffer, CaError *error) {
    Buffer out = {0};
    size_t n = strlen(a);
    if (buf_append(&out, a, n, error) < 0 ||
        (n && a[n - 1] != '/' && buf_append(&out, "/", 1, error) < 0) ||
        buf_append(&out, buffer, strlen(buffer), error) < 0) {
        buf_free(&out);
        return NULL;
    }
    return out.data;
}
char *path_absolute(const char *path, CaError *error) {
    if (!path || !*path) {
        ca_error(error, CA_ERROR, "路径不能为空");
        return NULL;
    }
    char *joined = NULL;
    if (*path == '/')
        joined = ca_strdup(path, error);
    else {
        char *cwd = getcwd(NULL, 0);
        if (!cwd) {
            ca_errno(error, "getcwd");
            return NULL;
        }
        joined = path_join(cwd, path, error);
        free(cwd);
    }
    if (!joined)
        return NULL;
    Buffer out = {0};
    if (buf_append(&out, "/", 1, error) < 0) {
        free(joined);
        return NULL;
    }
    char *save = NULL;
    for (char *part = strtok_r(joined, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, "."))
            continue;
        if (!strcmp(part, "..")) {
            while (out.len > 1 && out.data[out.len - 1] != '/')
                --out.len;
            if (out.len > 1)
                --out.len;
            out.data[out.len] = 0;
            continue;
        }
        if ((out.len > 1 && buf_append(&out, "/", 1, error) < 0) ||
            buf_append(&out, part, strlen(part), error) < 0) {
            free(joined);
            buf_free(&out);
            return NULL;
        }
    }
    free(joined);
    return out.data;
}
int mkdir_parents(const char *path, mode_t mode, CaError *error) {
    char *p = ca_strdup(path, error);
    if (!p)
        return -1;
    for (char *s = p + 1;; ++s) {
        if (*s && *s != '/')
            continue;
        char saved = *s;
        *s = 0;
        if (mkdir(p, mode) < 0) {
            struct stat st;
            if (errno != EEXIST || stat(p, &st) < 0 || !S_ISDIR(st.st_mode)) {
                free(p);
                return ca_errno(error, "创建目录");
            }
        }
        *s = saved;
        if (!saved)
            break;
    }
    free(p);
    return 0;
}
int read_file_bytes(const char *path, size_t limit, Buffer *out, CaError *error) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return ca_errno(error, "打开文件");
    char bytes[8192];
    size_t n;
    int code = 0;
    while ((n = fread(bytes, 1, sizeof(bytes), file)) > 0) {
        if (out->len > limit || n > limit - out->len) {
            code = ca_error(error, CA_ERROR, "文件过大");
            break;
        }
        if (buf_append(out, bytes, n, error) < 0) {
            code = -1;
            break;
        }
    }
    if (!code && ferror(file))
        code = ca_errno(error, "读取文件");
    fclose(file);
    return code;
}
int write_file_bytes(const char *path, const void *bytes, size_t n, CaError *error) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return ca_errno(error, "打开文件");
    int code = fwrite(bytes, 1, n, file) == n ? 0 : ca_errno(error, "写文件");
    if (fclose(file) != 0 && !code)
        code = ca_errno(error, "关闭文件");
    return code;
}
bool valid_cstring(const JVal *v) {
    return json_is_string(v) && !memchr(json_str(v), 0, json_len(v));
}
int parse_int_arg(const char *text, int *out, CaError *error) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (!*text || *end || errno || value < INT_MIN || value > INT_MAX)
        return ca_error(error, CA_ERROR, "非法整数参数：%s", text);
    *out = (int)value;
    return 0;
}
int ca_emit(const Callbacks *cb, const JVal *event, CaError *error) {
    return cb && cb->emit ? cb->emit(cb->emit_ctx, event, error) : 0;
}
bool ca_cancelled(const Callbacks *cb) {
    return stopping || (cb && cb->cancel && cb->cancel(cb->cancel_ctx));
}
void config_init(Config *c) {
    *c = (Config){.host = "127.0.0.1",
                  .port = 8888,
                  .model_url = "http://127.0.0.1:8080",
                  .model = "qwen3.5-4b",
                  .project = AGENT_PROJECT_DIR "/templates/c",
                  .sessions = AGENT_PROJECT_DIR "/sessions",
                  .context = 32768,
                  .input_limit = 24576,
                  .output_limit = 7680,
                  .max_rounds = 24,
                  .model_timeout = 180,
                  .task_timeout = 900,
                  .command_timeout = 30,
                  .max_clients = 8};
}
