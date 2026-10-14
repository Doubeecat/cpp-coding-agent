#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int socket, session_lock;
    const Config *config;
    char *id, *root, *request_id;
    Workspace workspace;
    Tools tools;
    FILE *events, *transcript;
    Json *history, *seen;
    ModelClient model;
    bool capacity_checked, resumed;
    int64_t deadline;
    Buffer partial;
} Session;
const char *system_prompt(void) {
    return "You are a software-engineering agent working in a checked-out repository.\n"
           "Use the available tools to inspect files, make focused edits, and run commands.\n"
           "The default empty project uses C11: inspect README.md and create .c files. Follow the "
           "actual repository's build instructions.\n"
           "Solve the task, validate when practical, then give a brief final answer.";
}
static int checked_json(const Json *j, CaError *error) {
    return j && !j->failed ? 0 : ca_error(error, CA_NOMEM, "JSON 内存分配失败");
}
static int session_remember(Session *session, const JVal *message, CaError *error) {
    if (json_append(session->history, message) < 0)
        return ca_error(error, CA_NOMEM, "历史内存不足");
    Json *item = json_object();
    char time[40];
    timestamp(time);
    json_set_string(item, "time", time);
    json_set_value(item, "message", message);
    int code = checked_json(item, error);
    if (!code)
        code = append_json(session->transcript, JV(item), error);
    json_free(item);
    return code;
}
static int session_emit(void *context, const JVal *value, CaError *error) {
    Session *session = context;
    Json *event = json_clone(value);
    char time[40];
    timestamp(time);
    json_set_string(event, "time", time);
    json_set_string(event, "session_id", session->id);
    json_set_string(event, "request_id", session->request_id ? session->request_id : "");
    int code = checked_json(event, error);
    if (!code)
        code = append_json(session->events, JV(event), error);
    if (!code)
        code = send_frame(session->socket, JV(event), 10000, error);
    json_free(event);
    return code;
}
static int emit_owned(Session *session, Json *event, CaError *error) {
    int code = checked_json(event, error);
    if (!code)
        code = session_emit(session, JV(event), error);
    json_free(event);
    return code;
}
static bool session_cancel(void *context) {
    Session *session = context;
    return stopping || peer_closed(session->socket) || monotonic_ms() >= session->deadline;
}
static int stream_emit(void *context, const JVal *event, CaError *error) {
    Session *session = context;
    if (!strcmp(json_string(event, "type", ""), "text_delta")) {
        JVal *text = json_get(event, "text");
        if (buf_append(&session->partial, json_str(text), json_len(text), error) < 0)
            return -1;
    }
    return session_emit(session, event, error);
}
typedef struct {
    Session *session;
    const JVal *id;
} ToolEmit;
static int tool_emit(void *context, const JVal *event, CaError *error) {
    ToolEmit *c = context;
    Json *detailed = json_clone(event);
    json_set_value(detailed, "tool_call_id", c->id);
    return emit_owned(c->session, detailed, error);
}
static FILE *open_log(const char *root, const char *name, CaError *error) {
    char *path = path_join(root, name, error);
    if (!path)
        return NULL;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(path);
    if (fd < 0) {
        ca_errno(error, "打开会话日志");
        return NULL;
    }
    FILE *file = fdopen(fd, "a");
    if (!file) {
        close(fd);
        ca_errno(error, "打开会话日志");
    }
    return file;
}
static int lock_session(const char *root, CaError *error) {
    char *path = path_join(root, ".session.lock", error);
    if (!path)
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    free(path);
    if (fd < 0)
        return ca_errno(error, "创建会话锁");
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return ca_error(error, CA_ERROR, "会话正在使用，无法同时恢复");
    }
    return fd;
}
static Json *workspace_ready(Session *session) {
    Json *ready = json_event("workspace_ready");
    json_set_string(ready, "session_id", session->id);
    json_set_string(ready, "workspace", session->workspace.root);
    json_set_string(ready, "model", session->config->model);
    return ready;
}
static int send_history(Session *session, CaError *error) {
    size_t length;
    char *text = json_dump(session->history, false, &length);
    if (!text)
        return ca_error(error, CA_NOMEM, "历史序列化失败");
    int code = 0;
    for (size_t offset = 0; offset < length && !code;) {
        size_t size = length - offset;
        if (size > 64 * 1024)
            size = utf8_prefix_length(text + offset, 64 * 1024, false);
        Json *event = json_event("session_history");
        json_set_string(event, "session_id", session->id);
        json_set_stringn(event, "text", text + offset, size);
        json_set_bool(event, "done", offset + size == length);
        code = checked_json(event, error);
        if (!code)
            code = send_frame(session->socket, JV(event), 10000, error);
        json_free(event);
        offset += size;
    }
    free(text);
    return code;
}
static int configure_workspace(Session *session, const JVal *request, CaError *error) {
    JVal *value = json_get(request, "workspace");
    if (!valid_cstring(value) || !json_len(value) || json_len(value) > 4096)
        return ca_error(error, CA_ERROR, "工作目录为空、过长或包含 NUL");
    char *path = realpath(json_str(value), NULL);
    if (!path)
        path = path_absolute(json_str(value), error);
    if (!path)
        return -1;
    if (strcmp(path, session->workspace.root)) {
        Workspace next;
        workspace_init(&next);
        Tools tools = {0};
        int code = -1;
        if (open_workspace(path, &next, error) == 0 &&
            tools_init(&tools, next.root, session->root, session->config->command_timeout, error) ==
                0 &&
            snapshot_save_workspace(session->root, next.root, error) == 0) {
            workspace_destroy(&session->workspace);
            session->workspace = next;
            workspace_init(&next);
            tools_destroy(&session->tools);
            session->tools = tools;
            memset(&tools, 0, sizeof(tools));
            code = 0;
        }
        workspace_destroy(&next);
        tools_destroy(&tools);
        free(path);
        if (code < 0)
            return -1;
    } else
        free(path);
    return emit_owned(session, workspace_ready(session), error);
}
static int switch_session(Session *session, const JVal *request, CaError *error) {
    JVal *value = json_get(request, "session_id");
    const char *id = json_str(value);
    if (!valid_cstring(value) || !json_len(value) || json_len(value) > 128 ||
        strspn(id, "0123456789-") != json_len(value))
        return ca_error(error, CA_ERROR, "会话 ID 格式不正确");
    if (!strcmp(id, session->id))
        return ca_error(error, CA_ERROR, "已经是当前会话");
    char *root = path_join(session->config->sessions, id, error), *next_id = NULL;
    struct stat st;
    Workspace next;
    workspace_init(&next);
    SessionSnapshot loaded = {0};
    Tools tools = {0};
    FILE *events = NULL, *transcript = NULL;
    int lock = -1, code = -1;
    Buffer warning = {0};
    bool restored = false;
    if (!root)
        goto done;
    if (lstat(root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        ca_error(error, CA_ERROR, "会话不存在：%s", id);
        goto done;
    }
    lock = lock_session(root, error);
    if (lock < 0 || snapshot_load(root, &loaded, error) < 0)
        goto done;
    if (loaded.workspace && *loaded.workspace) {
        char *canonical = realpath(loaded.workspace, NULL);
        if (canonical && !strcmp(canonical, session->workspace.root))
            restored = true;
        else {
            CaError local = {0};
            if (open_workspace(loaded.workspace, &next, &local) == 0)
                restored = true;
            else if (buf_printf(&warning, error, "原工作目录未恢复（%s），继续使用当前目录",
                                local.message) < 0) {
                free(canonical);
                goto done;
            }
        }
        free(canonical);
    }
    const char *workspace = next.root ? next.root : session->workspace.root;
    if (tools_init(&tools, workspace, root, session->config->command_timeout, error) < 0)
        goto done;
    events = open_log(root, "events.jsonl", error);
    transcript = open_log(root, "history.jsonl", error);
    next_id = ca_strdup(id, error);
    if (!events || !transcript || !next_id)
        goto done;
    if (restored && snapshot_save_workspace(root, workspace, error) < 0)
        goto done;
    fclose(session->events);
    fclose(session->transcript);
    close_fd(&session->session_lock);
    session->events = events;
    events = NULL;
    session->transcript = transcript;
    transcript = NULL;
    session->session_lock = lock;
    lock = -1;
    free(session->id);
    session->id = next_id;
    next_id = NULL;
    free(session->root);
    session->root = root;
    root = NULL;
    if (next.root) {
        workspace_destroy(&session->workspace);
        session->workspace = next;
        workspace_init(&next);
    }
    tools_destroy(&session->tools);
    session->tools = tools;
    memset(&tools, 0, sizeof(tools));
    json_free(session->history);
    session->history = loaded.history;
    loaded.history = NULL;
    free(session->request_id);
    session->request_id = NULL;
    model_destroy(&session->model);
    session->capacity_checked = false;
    session->resumed = true;
    Json *ready = json_event("session_ready");
    json_set_string(ready, "session_id", session->id);
    json_set_string(ready, "workspace", session->workspace.root);
    json_set_string(ready, "model", session->config->model);
    json_set_int(ready, "messages", (int64_t)loaded.messages);
    bool include_history = json_bool(request, "include_history", false);
    if (include_history)
        json_set_bool(ready, "history", true);
    if (warning.len)
        json_set_stringn(ready, "warning", warning.data, warning.len);
    code = emit_owned(session, ready, error);
    if (!code && include_history)
        code = send_history(session, error);
done:
    if (events)
        fclose(events);
    if (transcript)
        fclose(transcript);
    close_fd(&lock);
    free(root);
    free(next_id);
    workspace_destroy(&next);
    tools_destroy(&tools);
    snapshot_destroy(&loaded);
    buf_free(&warning);
    return code;
}
static Json *model_body(Session *session, bool generate, CaError *error) {
    Json *body =
        generate
            ? json_literal("{\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,\"stream\":"
                           "true,\"stream_options\":{\"include_usage\":true},\"temperature\":0.2,"
                           "\"top_p\":0.8,\"presence_penalty\":0.0,\"repeat_penalty\":1.0}")
            : json_object();
    Json *schema = tool_schema();
    json_set_string(body, "model", session->config->model);
    json_set_value(body, "messages", JV(session->history));
    json_set_value(body, "tools", JV(schema));
    if (generate)
        json_set_int(body, "max_tokens", session->config->output_limit);
    json_free(schema);
    if (checked_json(body, error) < 0) {
        json_free(body);
        return NULL;
    }
    return body;
}
static int usage_event(Session *session, int input, int64_t used, int round, CaError *error) {
    Json *event = json_event("context_usage");
    json_set_int(event, "input_tokens", input);
    json_set_int(event, "context_tokens", used);
    json_set_int(event, "context", session->config->context);
    json_set_int(event, "input_limit", session->config->input_limit);
    json_set_int(event, "round", round);
    return emit_owned(session, event, error);
}
static int remember_tool(Session *session, const JVal *call, const JVal *result, CaError *error) {
    char *content = json_dump_value(result, false, NULL);
    if (!content)
        return ca_error(error, CA_NOMEM, "JSON 序列化失败");
    Json *message = json_object();
    json_set_string(message, "role", "tool");
    json_set_value(message, "tool_call_id", json_get(call, "id"));
    json_set_string(message, "content", content);
    free(content);
    int code = checked_json(message, error);
    if (!code)
        code = session_remember(session, JV(message), error);
    json_free(message);
    return code;
}
static int remember_user(Session *session, const JVal *request, CaError *error) {
    Json *message = json_object();
    json_set_string(message, "role", "user");
    json_set_value(message, "content", json_get(request, "text"));
    int code = checked_json(message, error);
    if (!code)
        code = session_remember(session, JV(message), error);
    json_free(message);
    return code;
}
static int model_round(Session *session, const Callbacks *callbacks, int round, Json **pending,
                       CaError *error) {
    Json *body = model_body(session, true, error);
    ModelReply reply = {0};
    int code = -1, count = 0;
    if (!body)
        goto done;
    if (model_count_tokens(&session->model, JV(body), callbacks, &count, error) < 0 ||
        usage_event(session, count, count, round, error) < 0)
        goto done;
    if (count > session->config->input_limit ||
        (int64_t)count + session->config->output_limit + 512 > session->config->context) {
        ca_error(error, CA_CONTEXT_LIMIT, "完整对话超过本槽输入预算，已保留历史；请新建连接继续");
        goto done;
    }
    buf_clear(&session->partial);
    if (model_complete(&session->model, JV(body), callbacks, &reply, error) < 0)
        goto done;
    *pending = reply.calls;
    reply.calls = NULL;
    Json *assistant = json_object();
    json_set_string(assistant, "role", "assistant");
    json_set_stringn(assistant, "content", buf_text(&reply.text), reply.text.len);
    if (json_len(JV(*pending)))
        json_set_value(assistant, "tool_calls", JV(*pending));
    code = checked_json(assistant, error);
    if (!code)
        code = session_remember(session, JV(assistant), error);
    json_free(assistant);
    if (code < 0)
        goto done;
    code = -1;
    buf_clear(&session->partial);
    int64_t used;
    if (reply.input_tokens >= 0 && reply.output_tokens >= 0)
        used = (int64_t)reply.input_tokens + reply.output_tokens;
    else {
        json_free(body);
        body = model_body(session, false, error);
        if (!body)
            goto done;
        int measured;
        if (model_count_tokens(&session->model, JV(body), callbacks, &measured, error) < 0)
            goto done;
        used = measured;
    }
    code = usage_event(session, count, used, round, error);
done:
    json_free(body);
    reply_destroy(&reply);
    return code;
}
static int run_tool(Session *session, const JVal *call, Json **result, CaError *error) {
    JVal *function = json_get(call, "function");
    JVal *arguments = json_get(function, "arguments");
    const char *name = json_string(function, "name", "");
    Json *args = json_parse(json_str(arguments), json_len(arguments));
    if (!args) {
        *result = json_object();
        json_set_bool(*result, "ok", false);
        json_set_string(*result, "error", "工具参数 JSON 无效");
        return checked_json(*result, error);
    }
    Json *event = json_event("tool_start");
    json_set_string(event, "name", name);
    json_set_value(event, "args", JV(args));
    json_set_value(event, "tool_call_id", json_get(call, "id"));
    int code = emit_owned(session, event, error);
    if (!code) {
        ToolEmit context = {session, json_get(call, "id")};
        Callbacks callbacks = {.emit = tool_emit,
                               .emit_ctx = &context,
                               .cancel = session_cancel,
                               .cancel_ctx = session};
        code = tools_execute(&session->tools, name, JV(args), &callbacks, result, error);
    }
    json_free(args);
    return code;
}
static int run_tools(Session *session, Json *pending, CaError *error) {
    while (json_len(JV(pending))) {
        JVal *call = json_at(JV(pending), 0);
        Json *result = NULL;
        int code = run_tool(session, call, &result, error);
        if (!code)
            code = remember_tool(session, call, JV(result), error);
        if (code < 0) {
            json_free(result);
            return -1;
        }
        Json *event = json_event("tool_result");
        json_set_string(event, "name", json_string(json_get(call, "function"), "name", ""));
        json_set_value(event, "result", JV(result));
        json_set_value(event, "tool_call_id", json_get(call, "id"));
        json_free(result);
        json_remove_first(pending);
        if (emit_owned(session, event, error) < 0)
            return -1;
    }
    return 0;
}
static int abort_task(Session *session, const Json *pending, CaError *error) {
    CaError primary = *error, ignored = {0};
    if (session->partial.len) {
        Json *partial = json_object();
        json_set_string(partial, "role", "assistant");
        json_set_stringn(partial, "content", session->partial.data, session->partial.len);
        if (partial && !partial->failed)
            session_remember(session, JV(partial), &ignored);
        json_free(partial);
    }
    for (size_t i = 0; i < json_len(JV(pending)); ++i) {
        Json *result = json_literal("{\"ok\":false,\"error\":\"工具执行中止\"}");
        if (result)
            remember_tool(session, json_at(JV(pending), i), JV(result), &ignored);
        json_free(result);
    }
    if (!peer_closed(session->socket) && !stopping) {
        Json *event = json_event("error");
        json_set_string(event, "message", primary.message);
        if (emit_owned(session, event, &ignored) < 0) {
            *error = ignored;
            return -1;
        }
    }
    return 0;
}
static int finish_task(Session *session, const char *status, int rounds, CaError *error) {
    Json *done = json_event("task_done");
    json_set_string(done, "status", status);
    json_set_int(done, "rounds", rounds);
    if (!peer_closed(session->socket) && !stopping) {
        ca_clear_error(error);
        return emit_owned(session, done, error);
    }
    char time[40];
    timestamp(time);
    json_set_string(done, "time", time);
    json_set_string(done, "session_id", session->id);
    json_set_string(done, "request_id", session->request_id);
    int code = checked_json(done, error);
    if (!code)
        code = append_json(session->events, JV(done), error);
    json_free(done);
    return code;
}
static int process_task(Session *session, const JVal *request, CaError *error) {
    if (remember_user(session, request, error) < 0)
        return -1;
    session->deadline = monotonic_ms() + (int64_t)session->config->task_timeout * 1000;
    Callbacks callbacks = {
        .emit = stream_emit, .emit_ctx = session, .cancel = session_cancel, .cancel_ctx = session};
    Json *pending = NULL;
    int rounds = 0, code = -1;
    bool finished = false;
    const char *status = "failed";
    buf_clear(&session->partial);
    ca_clear_error(error);
    if (!session->model.initialized && model_init(&session->model, session->config, error) < 0)
        goto failed;
    if (!session->capacity_checked) {
        if (model_check_capacity(&session->model, &callbacks, error) < 0)
            goto failed;
        session->capacity_checked = true;
    }
    for (; rounds < session->config->max_rounds; ++rounds) {
        if (model_round(session, &callbacks, rounds + 1, &pending, error) < 0)
            goto failed;
        if (!json_len(JV(pending))) {
            finished = true;
            status = "success";
            ++rounds;
            break;
        }
        if (run_tools(session, pending, error) < 0)
            goto failed;
        json_free(pending);
        pending = NULL;
    }
    if (!finished) {
        status = "limit";
        Json *event = json_event("error");
        json_set_string(event, "message", "已达到模型调用次数上限，请检查日志或提出更小的任务");
        if (emit_owned(session, event, error) < 0)
            goto failed;
    }
    goto complete;
failed:
    if (error->code == CA_CONTEXT_LIMIT)
        status = "context_limit";
    if (session_cancel(session))
        status = monotonic_ms() >= session->deadline ? "timeout" : "cancelled";
    if (abort_task(session, pending, error) < 0)
        goto done;
complete:
    code = finish_task(session, status, rounds, error);
done:
    json_free(pending);
    buf_clear(&session->partial);
    return code;
}
static void session_destroy(Session *session) {
    if (session->events)
        fclose(session->events);
    if (session->transcript)
        fclose(session->transcript);
    close_fd(&session->session_lock);
    workspace_destroy(&session->workspace);
    tools_destroy(&session->tools);
    model_destroy(&session->model);
    json_free(session->history);
    json_free(session->seen);
    free(session->id);
    free(session->root);
    free(session->request_id);
    buf_free(&session->partial);
}
static int simple_error(Session *session, const char *type, const char *message, CaError *error) {
    Json *event = json_event(type);
    json_set_string(event, "message", message);
    return emit_owned(session, event, error);
}
int run_session(int socket, const Config *config, const char *session_id, CaError *error) {
    Session session = {.socket = socket, .session_lock = -1, .config = config};
    workspace_init(&session.workspace);
    int code = -1;
    char *workspace = NULL;
    Json *message = NULL;
    session.id = ca_strdup(session_id, error);
    session.root = path_join(config->sessions, session_id, error);
    if (!session.id || !session.root)
        goto done;
    session.session_lock = lock_session(session.root, error);
    workspace = path_join(session.root, "workspace", error);
    if (session.session_lock < 0 || !workspace ||
        copy_project(config->project, workspace, error) < 0 ||
        open_workspace(workspace, &session.workspace, error) < 0)
        goto done;
    session.events = open_log(session.root, "events.jsonl", error);
    session.transcript = open_log(session.root, "history.jsonl", error);
    session.history = json_array();
    session.seen = json_array();
    if (!session.events || !session.transcript || !session.history || !session.seen) {
        if (!error->code)
            ca_error(error, CA_NOMEM, "内存不足");
        goto done;
    }
    Json *system = json_object();
    json_set_string(system, "role", "system");
    json_set_string(system, "content", system_prompt());
    code = checked_json(system, error);
    if (!code)
        code = session_remember(&session, JV(system), error);
    json_free(system);
    if (code < 0)
        goto done;
    if (!snapshot_has_workspace(session.root) &&
        snapshot_save_workspace(session.root, session.workspace.root, error) < 0)
        goto done;
    Json *hello = json_event("hello");
    json_set_int(hello, "pid", getpid());
    json_set_string(hello, "workspace", session.workspace.root);
    json_set_string(hello, "model", config->model);
    json_set_string(hello, "logs", session.root);
    json_set_int(hello, "context", config->context);
    json_set_int(hello, "input_limit", config->input_limit);
    json_set_int(hello, "output_limit", config->output_limit);
    if (emit_owned(&session, hello, error) < 0 ||
        tools_init(&session.tools, session.workspace.root, session.root, config->command_timeout,
                   error) < 0)
        goto done;
    code = 0;
    while (!stopping) {
        ca_clear_error(error);
        int received = receive_frame(socket, &message, 3600000, error);
        if (received <= 0) {
            code = received < 0 ? -1 : 0;
            break;
        }
        const char *type = json_string(JV(message), "type", "");
        if (!strcmp(type, "quit"))
            break;
        if (!strcmp(type, "list_sessions") || !strcmp(type, "resume_session") ||
            !strcmp(type, "set_workspace")) {
            free(session.request_id);
            session.request_id = NULL;
            int control = -1;
            if (!strcmp(type, "list_sessions")) {
                Json *sessions = list_sessions(config->sessions, error),
                     *event = json_event("session_list");
                json_set_value(event, "sessions", JV(sessions));
                json_set_string(event, "current", session.id);
                json_free(sessions);
                control = emit_owned(&session, event, error);
            } else if (!strcmp(type, "resume_session"))
                control = switch_session(&session, JV(message), error);
            else if (json_len(JV(session.seen)) && !session.resumed)
                control =
                    ca_error(error, CA_ERROR, "工作目录只能在首次提问前指定，或先恢复一个会话");
            else
                control = configure_workspace(&session, JV(message), error);
            if (control < 0) {
                CaError local = {0};
                if (simple_error(&session,
                                 !strcmp(type, "resume_session") ? "session_error"
                                                                 : "workspace_error",
                                 error->message, &local) < 0) {
                    *error = local;
                    code = -1;
                    break;
                }
            }
            json_free(message);
            message = NULL;
            continue;
        }
        JVal *id = json_get(JV(message), "request_id"), *text = json_get(JV(message), "text");
        free(session.request_id);
        session.request_id =
            valid_cstring(id) ? ca_strdup(json_str(id), error) : ca_strdup("", error);
        if (!session.request_id) {
            code = -1;
            break;
        }
        const char *invalid = NULL;
        if (strcmp(type, "user_message") || !valid_cstring(id) || !json_len(id) ||
            json_len(id) > 64 || !json_is_string(text))
            invalid = "需要 user_message、唯一 request_id 和 text 字符串";
        if (!invalid) {
            bool repeated = false;
            for (size_t i = 0; i < json_len(JV(session.seen)); ++i)
                if (!strcmp(json_str(json_at(JV(session.seen), i)), session.request_id))
                    repeated = true;
            if (repeated || json_len(JV(session.seen)) >= 1024)
                invalid = "request_id 重复或本连接任务过多";
            else if (json_append_string(session.seen, session.request_id) < 0) {
                code = ca_error(error, CA_NOMEM, "内存不足");
                break;
            }
        }
        if (!invalid && (!json_len(text) || json_len(text) > 64 * 1024))
            invalid = "消息需为 1 到 65536 字节";
        if (invalid) {
            if (simple_error(&session, "error", invalid, error) < 0) {
                code = -1;
                break;
            }
            Json *event = json_event("task_done");
            json_set_string(event, "status", "failed");
            if (emit_owned(&session, event, error) < 0) {
                code = -1;
                break;
            }
        } else if (process_task(&session, JV(message), error) < 0) {
            code = -1;
            break;
        }
        json_free(message);
        message = NULL;
        if (peer_closed(socket))
            break;
    }
    if (session.events) {
        CaError ignored = {0};
        Json *closed = json_event("session_closed");
        char time[40];
        timestamp(time);
        json_set_string(closed, "time", time);
        json_set_string(closed, "session_id", session.id);
        if (closed && !closed->failed)
            append_json(session.events, JV(closed), &ignored);
        json_free(closed);
    }
done:
    free(workspace);
    json_free(message);
    session_destroy(&session);
    return code;
}
