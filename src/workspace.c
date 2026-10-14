#include "common.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void workspace_init(Workspace *workspace) {
    *workspace = (Workspace){.lock_fd = -1};
}
void workspace_destroy(Workspace *workspace) {
    close_fd(&workspace->lock_fd);
    free(workspace->root);
    workspace_init(workspace);
}
int open_workspace(const char *path, Workspace *workspace, CaError *error) {
    Workspace next;
    workspace_init(&next);
    if (!path || !*path)
        return ca_error(error, CA_ERROR, "工作目录不能为空");
    if (mkdir_parents(path, 0700, error) < 0)
        return -1;
    next.root = realpath(path, NULL);
    if (!next.root)
        return ca_errno(error, "规范化工作目录");
    char *name = path_join(next.root, ".coding-agent.lock", error);
    if (!name) {
        workspace_destroy(&next);
        return -1;
    }
    next.lock_fd = open(name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(name);
    if (next.lock_fd < 0) {
        workspace_destroy(&next);
        return ca_errno(error, "无法创建工作目录锁");
    }
    if (flock(next.lock_fd, LOCK_EX | LOCK_NB) < 0) {
        workspace_destroy(&next);
        return ca_error(error, CA_ERROR, "工作目录已被另一个会话使用，请选择其他目录");
    }
    workspace_destroy(workspace);
    *workspace = next;
    return 0;
}
static int copy_file(const char *target, const Buffer *content, mode_t mode, CaError *error) {
    int fd = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return ca_errno(error, "创建模板文件");
    int code = -1;
    if (fchmod(fd, mode & 0777) < 0) {
        ca_errno(error, "保留模板文件权限");
        goto done;
    }
    size_t offset = 0;
    while (offset < content->len) {
        ssize_t size = write(fd, content->data + offset, content->len - offset);
        if (size < 0 && errno == EINTR)
            continue;
        if (size <= 0) {
            ca_errno(error, "复制模板文件");
            goto done;
        }
        offset += (size_t)size;
    }
    code = 0;
done:
    close(fd);
    if (code < 0)
        unlink(target);
    return code;
}
static int copy_tree(const char *src, const char *dst, int depth, size_t *count, size_t *total,
                     CaError *error) {
    DIR *dir = opendir(src);
    if (!dir)
        return ca_errno(error, "打开模板目录");
    struct dirent *entry;
    int code = 0;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "build") ||
            !strcmp(name, ".git") || !strcmp(name, ".coding-agent.lock"))
            continue;
        char *source = path_join(src, name, error), *target = path_join(dst, name, error);
        struct stat st;
        if (!source || !target) {
            code = -1;
            goto item_done;
        }
        if (depth > 8 || ++*count > 128) {
            code = ca_error(error, CA_ERROR, "项目模板过大");
            goto item_done;
        }
        if (lstat(source, &st) < 0) {
            code = ca_errno(error, "读取模板文件信息");
            goto item_done;
        }
        if (S_ISLNK(st.st_mode)) {
            code = ca_error(error, CA_ERROR, "项目模板不能包含符号链接");
            goto item_done;
        }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir_parents(target, 0700, error) < 0 ||
                copy_tree(source, target, depth + 1, count, total, error) < 0)
                code = -1;
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 || (uint64_t)st.st_size > CA_FRAME_LIMIT ||
                (size_t)st.st_size > 4 * CA_FRAME_LIMIT - *total) {
                code = ca_error(error, CA_ERROR, "项目模板文件过大");
                goto item_done;
            }
            *total += (size_t)st.st_size;
            Buffer content = {0};
            if (read_file_bytes(source, CA_FRAME_LIMIT, &content, error) < 0 ||
                copy_file(target, &content, st.st_mode, error) < 0)
                code = -1;
            buf_free(&content);
        } else
            code = ca_error(error, CA_ERROR, "项目模板只支持普通文件和目录");
    item_done:
        free(source);
        free(target);
        if (code < 0)
            break;
    }
    closedir(dir);
    return code;
}
int copy_project(const char *src, const char *dst, CaError *error) {
    struct stat st;
    if (stat(src, &st) < 0 || !S_ISDIR(st.st_mode))
        return ca_error(error, CA_ERROR, "项目模板必须为目录");
    if (mkdir_parents(dst, 0700, error) < 0)
        return -1;
    size_t count = 0, total = 0;
    return copy_tree(src, dst, 0, &count, &total, error);
}
void snapshot_destroy(SessionSnapshot *snapshot) {
    free(snapshot->workspace);
    json_free(snapshot->history);
    memset(snapshot, 0, sizeof(*snapshot));
}
static bool answered_call(const JVal *history, size_t start, const char *id) {
    for (size_t i = start; i < json_len(history); ++i) {
        JVal *message = json_at(history, i);
        const char *role = json_string(message, "role", "");
        if (!strcmp(role, "assistant") || !strcmp(role, "user"))
            return false;
        if (!strcmp(json_string(message, "role", ""), "tool") &&
            !strcmp(json_string(message, "tool_call_id", ""), id))
            return true;
    }
    return false;
}
int snapshot_load(const char *root, SessionSnapshot *snapshot, CaError *error) {
    SessionSnapshot next = {0};
    int code = -1;
    char *marker = path_join(root, CA_WORKSPACE_MARKER, error),
         *log = path_join(root, "history.jsonl", error);
    FILE *file = NULL;
    char *line = NULL;
    size_t cap = 0;
    Json *repaired = NULL;
    if (!marker || !log)
        goto done;
    file = fopen(marker, "r");
    if (file) {
        ssize_t size = getline(&line, &cap, file);
        fclose(file);
        file = NULL;
        if (size > 0) {
            if (line[size - 1] == '\n')
                line[--size] = 0;
            if (size && !memchr(line, 0, (size_t)size)) {
                next.workspace = ca_strdup(line, error);
                if (!next.workspace)
                    goto done;
            }
        }
    }
    file = fopen(log, "r");
    if (!file) {
        ca_error(error, CA_ERROR, "会话缺少 history.jsonl，无法恢复");
        goto done;
    }
    next.history = json_array();
    if (!next.history) {
        ca_error(error, CA_NOMEM, "内存不足");
        goto done;
    }
    ssize_t size;
    while ((size = getline(&line, &cap, file)) >= 0) {
        if ((size_t)size > 8 * CA_FRAME_LIMIT) {
            ca_error(error, CA_ERROR, "历史消息过大");
            goto done;
        }
        Json *item = json_parse(line, (size_t)size);
        if (!item)
            continue;
        JVal *message = json_get(JV(item), "message");
        const char *role = json_string(message, "role", "");
        if (json_is_object(message) && (!strcmp(role, "system") || !strcmp(role, "user") ||
                                        !strcmp(role, "assistant") || !strcmp(role, "tool"))) {
            if (json_append(next.history, message) < 0) {
                json_free(item);
                ca_error(error, CA_NOMEM, "内存不足");
                goto done;
            }
            ++next.messages;
        }
        json_free(item);
    }
    if (ferror(file)) {
        ca_errno(error, "读取历史");
        goto done;
    }
    repaired = json_array();
    if (!repaired) {
        ca_error(error, CA_NOMEM, "内存不足");
        goto done;
    }
    for (size_t i = 0; i < json_len(JV(next.history)); ++i) {
        JVal *message = json_at(JV(next.history), i);
        if (json_append(repaired, message) < 0) {
            ca_error(error, CA_NOMEM, "内存不足");
            goto done;
        }
        if (strcmp(json_string(message, "role", ""), "assistant"))
            continue;
        JVal *calls = json_get(message, "tool_calls");
        for (size_t k = 0; k < json_len(calls); ++k) {
            const char *id = json_string(json_at(calls, k), "id", "");
            if (!*id || answered_call(JV(next.history), i + 1, id))
                continue;
            Json *result = json_object(), *tool = json_object();
            json_set_bool(result, "ok", false);
            json_set_string(result, "error", "会话中断，工具未执行");
            char *content = json_dump(result, false, NULL);
            json_set_string(tool, "role", "tool");
            json_set_string(tool, "tool_call_id", id);
            if (content)
                json_set_string(tool, "content", content);
            if (!content || !tool || tool->failed || json_append(repaired, JV(tool)) < 0)
                code = ca_error(error, CA_NOMEM, "内存不足");
            free(content);
            json_free(result);
            json_free(tool);
            if (error->code)
                goto done;
            ++next.messages;
        }
    }
    json_free(next.history);
    next.history = repaired;
    repaired = NULL;
    snapshot_destroy(snapshot);
    *snapshot = next;
    memset(&next, 0, sizeof(next));
    code = 0;
done:
    if (file)
        fclose(file);
    free(line);
    free(marker);
    free(log);
    json_free(repaired);
    snapshot_destroy(&next);
    return code;
}
int snapshot_save_workspace(const char *root, const char *workspace, CaError *error) {
    char *target = path_join(root, CA_WORKSPACE_MARKER, error),
         *canonical = realpath(workspace, NULL);
    Buffer temp = {0};
    int fd = -1, code = -1;
    if (!target || !canonical) {
        if (!error->code)
            ca_errno(error, "规范化工作目录");
        goto done;
    }
    if (buf_printf(&temp, error, "%s.tmp-XXXXXX", target) < 0)
        goto done;
    fd = mkstemp(temp.data);
    if (fd < 0) {
        ca_errno(error, "创建目录标记");
        goto done;
    }
    if (set_fd_flags(fd, false, error) < 0)
        goto done;
    Buffer text = {0};
    if (buf_printf(&text, error, "%s\n", canonical) < 0) {
        buf_free(&text);
        goto done;
    }
    size_t offset = 0;
    while (offset < text.len) {
        ssize_t n = write(fd, text.data + offset, text.len - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ca_errno(error, "写目录标记");
            buf_free(&text);
            goto done;
        }
        offset += (size_t)n;
    }
    buf_free(&text);
    if (fsync(fd) < 0 || rename(temp.data, target) < 0) {
        ca_errno(error, "保存目录标记");
        goto done;
    }
    code = 0;
done:
    if (code < 0 && fd >= 0 && temp.data)
        unlink(temp.data);
    close_fd(&fd);
    free(target);
    free(canonical);
    buf_free(&temp);
    return code;
}
bool snapshot_has_workspace(const char *root) {
    CaError error = {0};
    char *name = path_join(root, CA_WORKSPACE_MARKER, &error);
    bool result = name && access(name, F_OK) == 0;
    free(name);
    return result;
}
static int session_order(const void *a, const void *b) {
    const Json *left = *(Json *const *)a, *right = *(Json *const *)b;
    return strcmp(json_string(JV(right), "modified", ""), json_string(JV(left), "modified", ""));
}
Json *list_sessions(const char *root, CaError *error) {
    Json *result = json_array();
    if (!result) {
        ca_error(error, CA_NOMEM, "内存不足");
        return NULL;
    }
    DIR *dir = opendir(root);
    if (!dir)
        return result;
    Json **items = NULL;
    size_t count = 0;
    struct dirent *entry;
    bool failed = false;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        CaError local = {0};
        char *path = path_join(root, entry->d_name, &local);
        SessionSnapshot snapshot = {0};
        struct stat st;
        if (!path) {
            failed = true;
            break;
        }
        if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode) ||
            snapshot_load(path, &snapshot, &local) < 0) {
            free(path);
            snapshot_destroy(&snapshot);
            continue;
        }
        Json *item = json_object();
        json_set_string(item, "session_id", entry->d_name);
        json_set_int(item, "messages", (int64_t)snapshot.messages);
        if (snapshot.workspace)
            json_set_string(item, "workspace", snapshot.workspace);
        else
            json_set_null(item, "workspace");
        char *log = path_join(path, "history.jsonl", &local),
             *events = path_join(path, "events.jsonl", &local);
        if (log && stat(log, &st) == 0) {
            struct tm tm;
            char when[32];
            gmtime_r(&st.st_mtime, &tm);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
            json_set_string(item, "modified", when);
        } else
            json_set_null(item, "modified");
        json_set_bool(item, "active", events && access(events, F_OK) == 0);
        free(log);
        free(events);
        free(path);
        snapshot_destroy(&snapshot);
        if (!item || item->failed || count >= SIZE_MAX / sizeof(*items)) {
            json_free(item);
            failed = true;
            break;
        }
        Json **next = realloc(items, (count + 1) * sizeof(*items));
        if (!next) {
            json_free(item);
            failed = true;
            break;
        }
        items = next;
        items[count++] = item;
    }
    closedir(dir);
    if (!failed) {
        if (count > 1)
            qsort(items, count, sizeof(*items), session_order);
        for (size_t i = 0; i < count && i < 200; ++i)
            if (json_append(result, JV(items[i])) < 0) {
                failed = true;
                break;
            }
    }
    for (size_t i = 0; i < count; ++i)
        json_free(items[i]);
    free(items);
    if (failed) {
        json_free(result);
        ca_error(error, CA_NOMEM, "会话列表内存不足");
        return NULL;
    }
    return result;
}
