#include "common.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    const char *name, *type, *description;
    bool required;
} Parameter;
typedef struct {
    const char *name, *description;
    Parameter parameters[4];
    size_t count;
} ToolSpec;
static const ToolSpec tool_specs[] = {
    {"Read",
     "Read a text file. Returns numbered lines.",
     {{"file_path", "string", "Path to the file to read.", true},
      {"offset", "integer", "Optional 1-based starting line.", false},
      {"limit", "integer", "Optional maximum number of lines.", false}},
     3},
    {"Write",
     "Write a complete file, creating parent directories when needed.",
     {{"file_path", "string", "Path to write.", true},
      {"content", "string", "Full file contents.", true}},
     2},
    {"Edit",
     "Replace an exact string in a text file.",
     {{"file_path", "string", "Path to edit.", true},
      {"old_string", "string", "Exact text to replace.", true},
      {"new_string", "string", "Replacement text.", true},
      {"replace_all", "boolean", "Replace all original matches.", false}},
     4},
    {"Glob",
     "Find files by glob pattern.",
     {{"pattern", "string", "Relative glob pattern, e.g. **/*.c.", true},
      {"path", "string", "Optional directory to search from.", false}},
     2},
    {"Bash",
     "Run a shell command in the repository workdir.",
     {{"command", "string", "Command to run.", true},
      {"description", "string", "Short description of the command.", false},
      {"timeout", "number", "Optional timeout in seconds.", false}},
     3}};
Json *tool_schema(void) {
    Json *schema = json_array();
    if (!schema)
        return NULL;
    for (size_t i = 0; i < sizeof(tool_specs) / sizeof(*tool_specs); ++i) {
        const ToolSpec *spec = &tool_specs[i];
        JVal *tool = json_new_object(schema), *function = json_new_object(schema);
        JVal *parameters = json_new_object(schema), *properties = json_new_object(schema);
        JVal *required = json_new_array(schema);
        json_add_string(schema, tool, "type", "function", 8);
        json_add_string(schema, function, "name", spec->name, strlen(spec->name));
        json_add_string(schema, function, "description", spec->description,
                        strlen(spec->description));
        json_add_string(schema, parameters, "type", "object", 6);
        for (size_t k = 0; k < spec->count; ++k) {
            const Parameter *p = &spec->parameters[k];
            JVal *property = json_new_object(schema);
            json_add_string(schema, property, "type", p->type, strlen(p->type));
            json_add_string(schema, property, "description", p->description,
                            strlen(p->description));
            json_add(schema, properties, p->name, property);
            if (p->required &&
                !yyjson_mut_arr_append(required, yyjson_mut_strcpy(schema->doc, p->name)))
                schema->failed = true;
        }
        json_add(schema, parameters, "properties", properties);
        json_add(schema, parameters, "required", required);
        json_add(schema, parameters, "additionalProperties", yyjson_mut_bool(schema->doc, false));
        json_add(schema, function, "parameters", parameters);
        json_add(schema, tool, "function", function);
        if (!yyjson_mut_arr_append(schema->root, tool))
            schema->failed = true;
    }
    if (schema->failed) {
        json_free(schema);
        return NULL;
    }
    return schema;
}
void tools_destroy(Tools *tools) {
    free(tools->root);
    free(tools->logs);
    memset(tools, 0, sizeof(*tools));
}
int tools_init(Tools *tools, const char *root, const char *logs, int timeout, CaError *error) {
    Tools next = {0};
    next.root = realpath(root, NULL);
    next.logs = ca_strdup(logs, error);
    next.timeout = timeout;
    if (!next.root || !next.logs) {
        tools_destroy(&next);
        if (!error->code)
            ca_errno(error, "初始化工具目录");
        return -1;
    }
    DIR *dir = opendir(logs);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            const char *name = entry->d_name;
            if (strncmp(name, "command-", 8))
                continue;
            char *end = NULL;
            errno = 0;
            unsigned long n = strtoul(name + 8, &end, 10);
            if (!errno && end != name + 8 && !strcmp(end, ".log") && n > next.commands)
                next.commands = n;
        }
        closedir(dir);
    }
    tools_destroy(tools);
    *tools = next;
    return 0;
}
static char *checked_path(const Tools *tools, const JVal *value, CaError *error) {
    if (!valid_cstring(value) || !json_len(value)) {
        ca_error(error, CA_ERROR, "文件路径为空或包含 NUL");
        return NULL;
    }
    const char *input = json_str(value);
    char *joined = *input == '/' ? ca_strdup(input, error) : path_join(tools->root, input, error);
    char *path = joined ? path_absolute(joined, error) : NULL;
    free(joined);
    if (!path)
        return NULL;
    size_t n = strlen(tools->root);
    if (strncmp(path, tools->root, n) || (n > 1 && path[n] && path[n] != '/')) {
        free(path);
        ca_error(error, CA_ERROR, "文件路径超出工作目录");
        return NULL;
    }
    const char *relative = path + n;
    if (*relative == '/')
        ++relative;
    if (!strcmp(relative, ".coding-agent.lock")) {
        free(path);
        ca_error(error, CA_ERROR, "工作目录锁由服务端管理");
        return NULL;
    }
    for (char *p = path + n; *p; ++p) {
        if (*p != '/' && p[1])
            continue;
        char *end = *p == '/' ? p : p + 1;
        char saved = *end;
        *end = 0;
        struct stat st;
        int status = lstat(path, &st);
        int saved_errno = errno;
        *end = saved;
        if (status == 0 && S_ISLNK(st.st_mode)) {
            free(path);
            ca_error(error, CA_ERROR, "不允许访问符号链接");
            return NULL;
        }
        if (status < 0 && saved_errno != ENOENT) {
            free(path);
            errno = saved_errno;
            ca_errno(error, "读取路径信息");
            return NULL;
        }
    }
    return path;
}
static int read_text(const char *path, Buffer *content, CaError *error) {
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > CA_FRAME_LIMIT)
        return ca_error(error, CA_ERROR, "文件不存在、不是普通文件或超过 1 MiB");
    return read_file_bytes(path, CA_FRAME_LIMIT, content, error);
}
static int atomic_write(const char *path, const char *bytes, size_t n, CaError *error) {
    if (n > 64 * 1024)
        return ca_error(error, CA_ERROR, "文件内容超过 64 KiB");
    struct stat st;
    bool exists = lstat(path, &st) == 0;
    if (exists && !S_ISREG(st.st_mode))
        return ca_error(error, CA_ERROR, "目标不是普通文件");
    char *parent = ca_strdup(path, error);
    if (!parent)
        return -1;
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent)
        *slash = 0;
    else if (slash)
        slash[1] = 0;
    int code = mkdir_parents(parent, 0700, error);
    free(parent);
    if (code < 0)
        return -1;
    Buffer name = {0};
    int fd = -1;
    code = -1;
    if (buf_printf(&name, error, "%s.tmp-XXXXXX", path) < 0)
        goto done;
    fd = mkstemp(name.data);
    if (fd < 0) {
        ca_errno(error, "创建临时文件");
        goto done;
    }
    if (set_fd_flags(fd, false, error) < 0 || (exists && fchmod(fd, st.st_mode & 0777) < 0)) {
        if (!error->code)
            ca_errno(error, "保留文件权限");
        goto done;
    }
    size_t offset = 0;
    while (offset < n) {
        ssize_t wrote = write(fd, bytes + offset, n - offset);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            ca_errno(error, "写文件");
            goto done;
        }
        offset += (size_t)wrote;
    }
    if (fsync(fd) < 0 || rename(name.data, path) < 0) {
        ca_errno(error, "保存文件");
        goto done;
    }
    code = 0;
done:
    if (code < 0 && fd >= 0)
        unlink(name.data);
    close_fd(&fd);
    buf_free(&name);
    return code;
}
static int validate(const char *name, const JVal *args, CaError *error) {
    if (!json_is_object(args))
        return ca_error(error, CA_ERROR, "工具参数必须为 JSON 对象");
    const ToolSpec *spec = NULL;
    for (size_t i = 0; i < sizeof(tool_specs) / sizeof(*tool_specs); ++i)
        if (!strcmp(tool_specs[i].name, name))
            spec = &tool_specs[i];
    if (!spec)
        return ca_error(error, CA_ERROR, "未知工具：%s", name);
    for (size_t i = 0; i < spec->count; ++i)
        if (spec->parameters[i].required && !json_get(args, spec->parameters[i].name))
            return ca_error(error, CA_ERROR, "缺少参数：%s", spec->parameters[i].name);
    JVal *key, *value;
    size_t i, n;
    yyjson_mut_obj_foreach((JVal *)args, i, n, key, value) {
        const Parameter *p = NULL;
        for (size_t k = 0; k < spec->count; ++k)
            if (valid_cstring(key) && !strcmp(spec->parameters[k].name, json_str(key)))
                p = &spec->parameters[k];
        if (!p)
            return ca_error(error, CA_ERROR, "未知参数：%s", json_str(key));
        bool valid = !strcmp(p->type, "string")    ? json_is_string(value)
                     : !strcmp(p->type, "integer") ? json_is_int(value)
                     : !strcmp(p->type, "boolean") ? json_is_bool(value)
                                                   : json_is_number(value);
        if (!valid)
            return ca_error(error, CA_ERROR, "参数类型无效：%s", p->name);
    }
    return 0;
}
static int do_read(Tools *tools, const JVal *args, Json **out, CaError *error) {
    JVal *offset = json_get(args, "offset"), *limit = json_get(args, "limit");
    int64_t start = offset ? json_integer(offset, -1) : 1;
    int64_t count = limit ? json_integer(limit, -1) : 200;
    if (start < 1 || start > INT_MAX || count < 1 || count > INT_MAX)
        return ca_error(error, CA_ERROR, "offset/limit 必须为正整数且不超过 INT_MAX");
    char *path = checked_path(tools, json_get(args, "file_path"), error);
    Buffer content = {0}, text = {0};
    int code = -1;
    if (!path || read_text(path, &content, error) < 0)
        goto done;
    bool truncated = false, line_truncated = false;
    int64_t number = 0, last = start - 1;
    size_t position = 0;
    while (position < content.len) {
        size_t end = position;
        while (end < content.len && content.data[end] != '\n')
            ++end;
        ++number;
        if (number >= start) {
            if (number >= start + count) {
                truncated = true;
                break;
            }
            Buffer entry = {0};
            if (buf_printf(&entry, error, "%lld: ", (long long)number) < 0 ||
                buf_append(&entry, content.data + position, end - position, error) < 0 ||
                buf_append(&entry, "\n", 1, error) < 0) {
                buf_free(&entry);
                goto done;
            }
            if (entry.len > 4096 - text.len) {
                truncated = true;
                if (!text.len) {
                    size_t prefix = utf8_prefix_length(entry.data, 4096, false);
                    if (buf_append(&text, entry.data, prefix, error) < 0) {
                        buf_free(&entry);
                        goto done;
                    }
                    last = number;
                    line_truncated = true;
                }
                buf_free(&entry);
                break;
            }
            if (buf_append(&text, entry.data, entry.len, error) < 0) {
                buf_free(&entry);
                goto done;
            }
            buf_free(&entry);
            last = number;
        }
        position = end < content.len ? end + 1 : end;
    }
    *out = json_object();
    json_set_bool(*out, "ok", true);
    json_set_value(*out, "file_path", json_get(args, "file_path"));
    json_set_stringn(*out, "content", buf_text(&text), text.len);
    json_set_bool(*out, "truncated", truncated);
    json_set_bool(*out, "line_truncated", line_truncated);
    json_set_int(*out, "next_offset", last + 1);
    code = 0;
done:
    free(path);
    buf_free(&content);
    buf_free(&text);
    return code;
}
static size_t find_bytes(const char *text, size_t n, const char *needle, size_t length,
                         size_t from) {
    if (!length || from > n || length > n - from)
        return SIZE_MAX;
    for (size_t i = from; i <= n - length; ++i)
        if (text[i] == needle[0] && !memcmp(text + i, needle, length))
            return i;
    return SIZE_MAX;
}
static int do_write_edit(Tools *tools, const char *name, const JVal *args, Json **out,
                         CaError *error) {
    char *path = checked_path(tools, json_get(args, "file_path"), error);
    if (!path)
        return -1;
    Buffer original = {0}, updated = {0};
    size_t matches = 0;
    int code = -1;
    const JVal *value = json_get(args, "content");
    const char *text = json_str(value);
    size_t n = json_len(value);
    if (!strcmp(name, "Edit")) {
        if (read_text(path, &original, error) < 0)
            goto done;
        JVal *old = json_get(args, "old_string"), *replacement = json_get(args, "new_string");
        size_t old_len = json_len(old);
        if (!old_len) {
            ca_error(error, CA_ERROR, "old_string 不能为空");
            goto done;
        }
        size_t pos = 0;
        while ((pos = find_bytes(buf_text(&original), original.len, json_str(old), old_len, pos)) !=
               SIZE_MAX) {
            ++matches;
            pos += old_len;
        }
        if (!matches) {
            ca_error(error, CA_ERROR, "old_string 未匹配任何内容");
            goto done;
        }
        if (!json_bool(args, "replace_all", false) && matches != 1) {
            ca_error(error, CA_ERROR, "old_string 匹配多处；请提供唯一内容或设置 replace_all=true");
            goto done;
        }
        size_t begin = 0;
        while ((pos = find_bytes(buf_text(&original), original.len, json_str(old), old_len,
                                 begin)) != SIZE_MAX) {
            if (buf_append(&updated, original.data + begin, pos - begin, error) < 0 ||
                buf_append(&updated, json_str(replacement), json_len(replacement), error) < 0)
                goto done;
            if (updated.len > 64 * 1024) {
                ca_error(error, CA_ERROR, "编辑后的文件超过 64 KiB");
                goto done;
            }
            begin = pos + old_len;
        }
        if (buf_append(&updated, original.data + begin, original.len - begin, error) < 0)
            goto done;
        text = buf_text(&updated);
        n = updated.len;
    }
    if (atomic_write(path, text, n, error) < 0)
        goto done;
    *out = json_object();
    json_set_bool(*out, "ok", true);
    json_set_value(*out, "file_path", json_get(args, "file_path"));
    json_set_int(*out, "bytes", (int64_t)n);
    if (!strcmp(name, "Edit"))
        json_set_int(*out, "replacements", (int64_t)matches);
    code = 0;
done:
    free(path);
    buf_free(&original);
    buf_free(&updated);
    return code;
}
typedef struct {
    char **parts;
    size_t count;
    char *storage;
} Parts;
static void parts_free(Parts *p) {
    free(p->parts);
    free(p->storage);
    memset(p, 0, sizeof(*p));
}
static int parts_init(Parts *p, const char *text, CaError *error) {
    p->storage = ca_strdup(text, error);
    if (!p->storage)
        return -1;
    char *save = NULL;
    for (char *part = strtok_r(p->storage, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, "."))
            continue;
        char **next = realloc(p->parts, (p->count + 1) * sizeof(*next));
        if (!next)
            return ca_error(error, CA_NOMEM, "内存不足");
        p->parts = next;
        p->parts[p->count++] = part;
    }
    return 0;
}
static int glob_matches(const Parts *pattern, const char *path, bool *match, CaError *error) {
    Parts parts = {0};
    bool *previous = NULL, *next = NULL;
    int code = -1;
    if (parts_init(&parts, path, error) < 0)
        goto done;
    previous = calloc(parts.count + 1, sizeof(bool));
    next = calloc(parts.count + 1, sizeof(bool));
    if (!previous || !next) {
        ca_error(error, CA_NOMEM, "内存不足");
        goto done;
    }
    previous[0] = true;
    for (size_t k = 0; k < pattern->count; ++k) {
        memset(next, 0, (parts.count + 1) * sizeof(bool));
        bool recursive = !strcmp(pattern->parts[k], "**");
        if (recursive)
            next[0] = previous[0];
        for (size_t i = 1; i <= parts.count; ++i)
            next[i] = recursive ? previous[i] || next[i - 1]
                                : previous[i - 1] &&
                                      fnmatch(pattern->parts[k], parts.parts[i - 1], 0) == 0;
        bool *swap = previous;
        previous = next;
        next = swap;
    }
    *match = previous[parts.count];
    code = 0;
done:
    parts_free(&parts);
    free(previous);
    free(next);
    return code;
}
typedef struct {
    const Tools *tools;
    const Parts *pattern;
    const Callbacks *callbacks;
    char *base;
    char **files;
    size_t count, bytes;
    bool truncated;
} Glob;
static int glob_walk(Glob *g, const char *dirpath, CaError *error) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        if (errno == EACCES)
            return 0;
        return ca_errno(error, "打开搜索目录");
    }
    struct dirent *entry;
    int code = 0;
    while (!g->truncated && (entry = readdir(dir))) {
        if (ca_cancelled(g->callbacks)) {
            code = ca_error(error, CA_CANCELLED, "文件搜索取消或超时");
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git") ||
            !strcmp(name, ".coding-agent.lock"))
            continue;
        char *path = path_join(dirpath, name, error);
        struct stat st;
        if (!path) {
            code = -1;
            break;
        }
        if (lstat(path, &st) < 0) {
            free(path);
            continue;
        }
        if (S_ISDIR(st.st_mode))
            code = glob_walk(g, path, error);
        else if (S_ISREG(st.st_mode)) {
            const char *local = path + strlen(g->base);
            if (*local == '/')
                ++local;
            bool matches = false;
            if (glob_matches(g->pattern, local, &matches, error) < 0)
                code = -1;
            else if (matches) {
                const char *relative = path + strlen(g->tools->root);
                if (*relative == '/')
                    ++relative;
                Json *string = json_object();
                if (string)
                    string->root = yyjson_mut_strcpy(string->doc, relative);
                size_t encoded = 0;
                char *serialized = json_dump(string, false, &encoded);
                json_free(string);
                if (!serialized)
                    code = ca_error(error, CA_NOMEM, "内存不足");
                else if (g->count == 100 || encoded + 1 > 4096 - g->bytes)
                    g->truncated = true;
                else {
                    char **next = realloc(g->files, (g->count + 1) * sizeof(*next));
                    if (!next)
                        code = ca_error(error, CA_NOMEM, "内存不足");
                    else {
                        g->files = next;
                        g->files[g->count] = ca_strdup(relative, error);
                        if (!g->files[g->count])
                            code = -1;
                        else {
                            ++g->count;
                            g->bytes += encoded + 1;
                        }
                    }
                }
                free(serialized);
            }
        }
        free(path);
        if (code < 0)
            break;
    }
    closedir(dir);
    return code;
}
static int string_order(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int do_glob(Tools *tools, const JVal *args, const Callbacks *cb, Json **out,
                   CaError *error) {
    JVal *pattern_value = json_get(args, "pattern");
    if (!valid_cstring(pattern_value) || !json_len(pattern_value) ||
        json_len(pattern_value) > 4096 || json_str(pattern_value)[0] == '/')
        return ca_error(error, CA_ERROR, "glob pattern 需为有效的相对模式");
    Parts pattern = {0};
    Glob g = {.tools = tools, .pattern = &pattern, .callbacks = cb};
    int code = -1;
    Json *default_path = NULL, *files = NULL;
    if (parts_init(&pattern, json_str(pattern_value), error) < 0)
        goto done;
    for (size_t i = 0; i < pattern.count; ++i)
        if (!strcmp(pattern.parts[i], "..")) {
            ca_error(error, CA_ERROR, "glob pattern 不允许 ..");
            goto done;
        }
    JVal *base = json_get(args, "path");
    if (!base) {
        default_path = json_literal("\".\"");
        base = JV(default_path);
    }
    g.base = checked_path(tools, base, error);
    if (!g.base)
        goto done;
    struct stat st;
    if (stat(g.base, &st) < 0 || !S_ISDIR(st.st_mode)) {
        ca_error(error, CA_ERROR, "搜索路径不是目录");
        goto done;
    }
    if (glob_walk(&g, g.base, error) < 0)
        goto done;
    if (g.count > 1)
        qsort(g.files, g.count, sizeof(*g.files), string_order);
    files = json_array();
    for (size_t i = 0; i < g.count; ++i)
        if (json_append_string(files, g.files[i]) < 0) {
            ca_error(error, CA_NOMEM, "内存不足");
            goto done;
        }
    *out = json_object();
    json_set_bool(*out, "ok", true);
    json_set_value(*out, "files", JV(files));
    json_set_bool(*out, "truncated", g.truncated);
    code = 0;
done:
    for (size_t i = 0; i < g.count; ++i)
        free(g.files[i]);
    free(g.files);
    free(g.base);
    parts_free(&pattern);
    json_free(default_path);
    json_free(files);
    return code;
}
static void command_cleanup(pid_t child, bool exited, int *status) {
    kill(-child, SIGKILL);
    if (!exited)
        while (waitpid(child, status, 0) < 0 && errno == EINTR) {
        }
}
static int emit_output(const Callbacks *cb, Buffer *pending, bool final, CaError *error) {
    size_t n = utf8_prefix_length(buf_text(pending), pending->len, final);
    if (!n)
        return 0;
    Json *event = json_event("tool_output");
    json_set_stringn(event, "text", buf_text(pending), n);
    if (!event || event->failed) {
        json_free(event);
        return ca_error(error, CA_NOMEM, "内存不足");
    }
    int code = ca_emit(cb, JV(event), error);
    json_free(event);
    if (!code)
        buf_consume(pending, n);
    return code;
}
static int do_bash(Tools *tools, const JVal *args, const Callbacks *cb, Json **out,
                   CaError *error) {
    JVal *command = json_get(args, "command");
    if (!valid_cstring(command) || !json_len(command))
        return ca_error(error, CA_ERROR, "command 为空或包含 NUL");
    double seconds = json_number(args, "timeout", tools->timeout);
    int64_t now = monotonic_ms();
    double milliseconds = seconds * 1000;
    if (!isfinite(milliseconds) || seconds <= 0 || milliseconds >= (double)INT64_MAX ||
        (uint64_t)milliseconds > (uint64_t)(INT64_MAX - now))
        return ca_error(error, CA_ERROR, "timeout 必须为有限的正秒数");
    int64_t deadline = now + (int64_t)milliseconds;
    int ends[2] = {-1, -1}, nullfd = -1, logfd = -1, code = -1, status = 0;
    pid_t child = -1;
    bool exited = false, eof = false, timed = false;
    FILE *full = NULL;
    Buffer logname = {0}, feedback = {0}, head = {0}, tail = {0}, pending = {0};
    size_t total = 0;
    if (pipe(ends) < 0)
        return ca_errno(error, "pipe");
    if (set_fd_flags(ends[0], true, error) < 0 || set_fd_flags(ends[1], false, error) < 0)
        goto done;
    for (;;) {
        if (tools->commands == ULONG_MAX) {
            ca_error(error, CA_ERROR, "命令日志编号耗尽");
            goto done;
        }
        buf_clear(&logname);
        if (buf_printf(&logname, error, "%s/command-%lu.log", tools->logs, ++tools->commands) < 0)
            goto done;
        logfd = open(logname.data, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (logfd >= 0)
            break;
        if (errno != EEXIST) {
            ca_errno(error, "创建命令日志");
            goto done;
        }
    }
    full = fdopen(logfd, "wb");
    if (!full) {
        ca_errno(error, "打开命令日志");
        goto done;
    }
    logfd = -1;
    nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nullfd < 0) {
        ca_errno(error, "打开 /dev/null");
        goto done;
    }
    child = fork();
    if (child < 0) {
        ca_errno(error, "fork 命令进程");
        goto done;
    }
    if (!child) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(tools->root) < 0 || dup2(nullfd, STDIN_FILENO) < 0 ||
            dup2(ends[1], STDOUT_FILENO) < 0 || dup2(ends[1], STDERR_FILENO) < 0)
            _exit(126);
        close(ends[0]);
        close(ends[1]);
        setenv("LC_ALL", "C", 1);
        execl("/bin/bash", "bash", "-c", json_str(command), (char *)NULL);
        _exit(127);
    }
    setpgid(child, child);
    close_fd(&ends[1]);
    while (!eof || !exited) {
        if (ca_cancelled(cb)) {
            ca_error(error, CA_CANCELLED, "命令任务取消或超时");
            goto done;
        }
        if (monotonic_ms() >= deadline) {
            timed = true;
            break;
        }
        struct pollfd descriptor = {ends[0], POLLIN, 0};
        if (poll(&descriptor, 1, 50) < 0 && errno != EINTR) {
            ca_errno(error, "命令输出 poll");
            goto done;
        }
        if (descriptor.revents & (POLLIN | POLLHUP)) {
            char bytes[2048];
            ssize_t n = read(ends[0], bytes, sizeof(bytes));
            if (n > 0) {
                size_t size = (size_t)n;
                if (size > 4 * CA_FRAME_LIMIT - total) {
                    ca_error(error, CA_ERROR, "命令输出超过 4 MiB，已停止");
                    goto done;
                }
                if (fwrite(bytes, 1, size, full) != size || fflush(full) < 0) {
                    ca_errno(error, "写命令日志");
                    goto done;
                }
                total += size;
                size_t remaining = feedback.len < 4096 ? 4096 - feedback.len : 0;
                if (buf_append(&feedback, bytes, size < remaining ? size : remaining, error) < 0)
                    goto done;
                remaining = head.len < 1900 ? 1900 - head.len : 0;
                if (buf_append(&head, bytes, size < remaining ? size : remaining, error) < 0 ||
                    buf_append(&tail, bytes, size, error) < 0 ||
                    buf_append(&pending, bytes, size, error) < 0)
                    goto done;
                if (tail.len > 1900)
                    buf_consume(&tail, tail.len - 1900);
                if (emit_output(cb, &pending, false, error) < 0)
                    goto done;
            } else if (!n)
                eof = true;
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                ca_errno(error, "读取命令输出");
                goto done;
            }
        }
        if (!exited) {
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child)
                exited = true;
            else if (result < 0 && errno != EINTR) {
                ca_errno(error, "waitpid");
                goto done;
            }
        }
    }
    command_cleanup(child, exited, &status);
    child = -1;
    if (emit_output(cb, &pending, true, error) < 0)
        goto done;
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                      : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    const char *basename = strrchr(buf_text(&logname), '/');
    basename = basename ? basename + 1 : buf_text(&logname);
    if (total > 4096) {
        buf_clear(&feedback);
        if (buf_append(&feedback, buf_text(&head), head.len, error) < 0 ||
            buf_printf(&feedback, error, "\n... [output truncated; see %s] ...\n", basename) < 0 ||
            buf_append(&feedback, buf_text(&tail), tail.len, error) < 0)
            goto done;
    }
    *out = json_object();
    json_set_bool(*out, "ok", !timed && exit_code == 0);
    json_set_int(*out, "exit_code", exit_code);
    json_set_bool(*out, "timed_out", timed);
    json_set_stringn(*out, "output", buf_text(&feedback), feedback.len);
    json_set_bool(*out, "output_truncated", total > 4096);
    json_set_string(*out, "log_file", basename);
    code = 0;
done:
    if (child > 0)
        command_cleanup(child, exited, &status);
    close_fd(&ends[0]);
    close_fd(&ends[1]);
    close_fd(&nullfd);
    close_fd(&logfd);
    if (full)
        fclose(full);
    buf_free(&logname);
    buf_free(&feedback);
    buf_free(&head);
    buf_free(&tail);
    buf_free(&pending);
    return code;
}
int tools_execute(Tools *tools, const char *name, const JVal *args, const Callbacks *cb, Json **out,
                  CaError *error) {
    *out = NULL;
    int code = -1;
    ca_clear_error(error);
    if (ca_cancelled(cb))
        return ca_error(error, CA_CANCELLED, "任务取消或超时");
    if (validate(name, args, error) == 0) {
        if (!strcmp(name, "Read"))
            code = do_read(tools, args, out, error);
        else if (!strcmp(name, "Write") || !strcmp(name, "Edit"))
            code = do_write_edit(tools, name, args, out, error);
        else if (!strcmp(name, "Glob"))
            code = do_glob(tools, args, cb, out, error);
        else
            code = do_bash(tools, args, cb, out, error);
    }
    if (code < 0) {
        if (error->code == CA_CANCELLED || error->code == CA_NOMEM)
            return -1;
        json_free(*out);
        *out = json_object();
        json_set_bool(*out, "ok", false);
        json_set_string(*out, "error", error->message);
        ca_clear_error(error);
    }
    if (!*out || (*out)->failed) {
        json_free(*out);
        *out = NULL;
        return ca_error(error, CA_NOMEM, "内存不足");
    }
    return 0;
}
