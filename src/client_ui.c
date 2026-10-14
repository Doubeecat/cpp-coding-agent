#include "client_ui.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int set_text(Buffer *out, const char *text, CaError *error) {
    buf_clear(out);
    return buf_append(out, text, strlen(text), error);
}
static void entry_destroy(UiEntry *entry) {
    buf_free(&entry->text);
    buf_free(&entry->name);
    buf_free(&entry->output);
    json_free(entry->args);
    json_free(entry->result);
}
static void entries_destroy(UiState *state) {
    for (size_t i = 0; i < state->count; ++i)
        entry_destroy(&state->entries[i]);
    free(state->entries);
    state->entries = NULL;
    state->count = state->capacity = 0;
}
int ui_init(UiState *state, CaError *error) {
    memset(state, 0, sizeof(*state));
    state->context = 32768;
    state->tokens = -1;
    state->input_limit = 24576;
    state->active_tool = -1;
    return set_text(&state->status, "就绪", error);
}
void ui_destroy(UiState *state) {
    entries_destroy(state);
    buf_free(&state->workspace);
    buf_free(&state->model);
    buf_free(&state->session);
    buf_free(&state->request_id);
    buf_free(&state->status);
    buf_free(&state->history);
    buf_free(&state->history_warning);
    memset(state, 0, sizeof(*state));
}
int ui_add(UiState *state, UiKind kind, const char *text, size_t len, CaError *error) {
    if (state->count == state->capacity) {
        size_t capacity = state->capacity ? state->capacity * 2 : 16;
        if (capacity < state->capacity || capacity > SIZE_MAX / sizeof(UiEntry))
            return ca_error(error, CA_NOMEM, "界面条目数量溢出");
        UiEntry *entries = realloc(state->entries, capacity * sizeof(*entries));
        if (!entries)
            return ca_error(error, CA_NOMEM, "界面内存不足");
        state->entries = entries;
        state->capacity = capacity;
    }
    UiEntry entry = {.kind = kind};
    if (buf_append(&entry.text, text, len, error) < 0)
        return -1;
    state->entries[state->count++] = entry;
    return 0;
}
static int notice(UiState *state, const char *text, CaError *error) {
    return ui_add(state, UI_NOTICE, text, strlen(text), error);
}
int ui_ask(UiState *state, const char *text, size_t len, const char *id, CaError *error) {
    if (set_text(&state->request_id, id, error) < 0 ||
        set_text(&state->status, "思考中", error) < 0 ||
        ui_add(state, UI_USER, text, len, error) < 0)
        return -1;
    state->busy = true;
    state->started = monotonic_ms();
    state->completed_seconds = 0;
    state->round = 0;
    state->active_tool = -1;
    return 0;
}
double ui_elapsed(const UiState *state) {
    return state->busy ? (monotonic_ms() - state->started) / 1000.0 : state->completed_seconds;
}
static int session_list(UiState *state, const JVal *event, CaError *error) {
    JVal *sessions = json_get(event, "sessions");
    Buffer line = {0};
    int code = -1;
    if (!json_len(sessions))
        return notice(state, "当前没有可恢复的会话", error);
    if (buf_printf(&line, error, "共 %zu 个会话", json_len(sessions)) < 0 ||
        notice(state, buf_text(&line), error) < 0)
        goto done;
    for (size_t i = 0; i < json_len(sessions); ++i) {
        JVal *item = json_at(sessions, i);
        const char *id = json_string(item, "session_id", "");
        const char *where = json_string(item, "workspace", ""),
                   *when = json_string(item, "modified", "");
        const char *slash = strrchr(where, '/');
        if (slash)
            where = slash + 1;
        buf_clear(&line);
        if (buf_printf(&line, error, "%s%s  %lld 条%s%s%s%s",
                       !strcmp(id, json_string(event, "current", "")) ? "* " : "  ", id,
                       (long long)json_int(item, "messages", 0), *when ? "  " : "", when,
                       *where ? "  " : "", where) < 0 ||
            notice(state, buf_text(&line), error) < 0)
            goto done;
    }
    code = 0;
done:
    buf_free(&line);
    return code;
}
static int metadata(UiState *state, const JVal *event, CaError *error) {
    const char *keys[] = {"workspace", "model", "session_id"};
    Buffer *fields[] = {&state->workspace, &state->model, &state->session};
    for (size_t i = 0; i < 3; ++i) {
        JVal *value = json_get(event, keys[i]);
        if (json_is_string(value) && set_text(fields[i], json_str(value), error) < 0)
            return -1;
    }
    state->context = (int)json_int(event, "context", state->context);
    state->input_limit = (int)json_int(event, "input_limit", state->input_limit);
    if (!strcmp(json_string(event, "type", ""), "session_ready")) {
        if (json_bool(event, "history", false)) {
            buf_clear(&state->history);
            state->restoring = true;
            if (set_text(&state->history_warning, json_string(event, "warning", ""), error) < 0)
                return -1;
            return set_text(&state->status, "正在恢复", error);
        }
        bool requested_history = state->restoring;
        state->restoring = false;
        buf_free(&state->history);
        buf_free(&state->history_warning);
        Buffer text = {0};
        int code = buf_printf(&text, error, "已恢复会话 %s，共 %lld 条消息",
                              buf_text(&state->session), (long long)json_int(event, "messages", 0));
        if (!code)
            code = notice(state, buf_text(&text), error);
        buf_free(&text);
        if (code < 0)
            return -1;
        if (requested_history &&
            notice(state, "服务端未提供旧对话，请重启更新后的 agent-server", error) < 0)
            return -1;
        if (json_get(event, "warning"))
            return notice(state, json_string(event, "warning", ""), error);
    }
    return 0;
}
static int start_tool(UiState *state, const JVal *event, CaError *error) {
    if (ui_add(state, UI_TOOL, "", 0, error) < 0)
        return -1;
    UiEntry *entry = &state->entries[state->count - 1];
    if (set_text(&entry->name, json_string(event, "name", "工具"), error) < 0)
        return -1;
    JVal *args = json_get(event, "args");
    entry->args = args ? json_clone(args) : json_object();
    if (!entry->args)
        return ca_error(error, CA_NOMEM, "界面内存不足");
    entry->running = true;
    state->active_tool = (int)state->count - 1;
    return set_text(&state->status,
                    !strcmp(buf_text(&entry->name), "Bash") ? "命令执行中" : "工具执行中", error);
}
static int tool_result(UiState *state, const JVal *event, CaError *error) {
    if (state->active_tool < 0 && start_tool(state, event, error) < 0)
        return -1;
    UiEntry *entry = &state->entries[state->active_tool];
    Json *result = json_clone(json_get(event, "result"));
    if (!result)
        return ca_error(error, CA_ERROR, "工具结果缺少 result");
    json_free(entry->result);
    entry->result = result;
    entry->running = false;
    state->active_tool = -1;
    return set_text(&state->status, "思考中", error);
}
static int task_done(UiState *state, const JVal *event, CaError *error) {
    state->restoring = false;
    buf_free(&state->history);
    buf_free(&state->history_warning);
    if (state->busy)
        state->completed_seconds = ui_elapsed(state);
    state->busy = false;
    const char *outcome = json_string(event, "status", "failed"), *label = outcome;
    const char *codes[] = {"success", "failed", "cancelled", "timeout", "context_limit", "limit"};
    const char *labels[] = {"完成", "失败", "已取消", "已超时", "上下文已满", "达到轮次上限"};
    for (size_t i = 0; i < 6; ++i)
        if (!strcmp(outcome, codes[i]))
            label = labels[i];
    if (strcmp(outcome, "success"))
        state->exit_code = 1;
    if (state->active_tool >= 0)
        state->entries[state->active_tool].running = false;
    state->active_tool = -1;
    return set_text(&state->status, label, error);
}
static Json *stored_json(const JVal *value, const char *raw_key) {
    Json *parsed =
        json_is_string(value) ? json_parse(json_str(value), json_len(value)) : json_clone(value);
    if (!parsed) {
        parsed = json_object();
        if (value)
            json_set_value(parsed, raw_key, value);
    }
    return parsed;
}
static const JVal *tool_content(const JVal *history, size_t start, const char *id) {
    for (size_t i = start; i < json_len(history); ++i) {
        JVal *message = json_at(history, i);
        const char *role = json_string(message, "role", "");
        if (!strcmp(role, "assistant") || !strcmp(role, "user"))
            break;
        if (!strcmp(role, "tool") && !strcmp(json_string(message, "tool_call_id", ""), id))
            return json_get(message, "content");
    }
    return NULL;
}
static int restore_tool(UiState *state, const JVal *call, const JVal *content, CaError *error) {
    if (ui_add(state, UI_TOOL, "", 0, error) < 0)
        return -1;
    UiEntry *entry = &state->entries[state->count - 1];
    JVal *function = json_get(call, "function");
    if (set_text(&entry->name, json_string(function, "name", "工具"), error) < 0)
        return -1;
    entry->args = stored_json(json_get(function, "arguments"), "raw_arguments");
    if (content)
        entry->result = stored_json(content, "output");
    if (!entry->args || entry->args->failed ||
        (content && (!entry->result || entry->result->failed)))
        return ca_error(error, CA_NOMEM, "恢复工具卡片失败");
    return 0;
}
static int restore_history(UiState *state, CaError *error) {
    Json *history = json_parse(buf_text(&state->history), state->history.len);
    UiState next = {0};
    Buffer text = {0};
    int code = -1;
    if (!history || !json_is_array(JV(history))) {
        ca_error(error, CA_ERROR, "会话历史格式不正确");
        goto done;
    }
    if (ui_init(&next, error) < 0)
        goto done;
    for (size_t i = 0; i < json_len(JV(history)); ++i) {
        JVal *message = json_at(JV(history), i), *content = json_get(message, "content");
        const char *role = json_string(message, "role", "");
        bool user = !strcmp(role, "user"), assistant = !strcmp(role, "assistant");
        if ((user || assistant) && json_is_string(content) && json_len(content) &&
            ui_add(&next, user ? UI_USER : UI_ASSISTANT, json_str(content), json_len(content),
                   error) < 0)
            goto done;
        JVal *calls = assistant ? json_get(message, "tool_calls") : NULL;
        for (size_t k = 0; k < json_len(calls); ++k) {
            JVal *call = json_at(calls, k);
            const JVal *result = tool_content(JV(history), i + 1, json_string(call, "id", ""));
            if (restore_tool(&next, call, result, error) < 0)
                goto done;
        }
    }
    if (buf_printf(&text, error, "已恢复会话 %s，共 %zu 条消息", buf_text(&state->session),
                   json_len(JV(history))) < 0 ||
        notice(&next, buf_text(&text), error) < 0)
        goto done;
    if (state->history_warning.len && notice(&next, buf_text(&state->history_warning), error) < 0)
        goto done;
    entries_destroy(state);
    state->entries = next.entries;
    state->count = next.count;
    state->capacity = next.capacity;
    next.entries = NULL;
    next.count = next.capacity = 0;
    Buffer status = state->status;
    state->status = next.status;
    next.status = status;
    state->busy = false;
    state->active_tool = state->tokens = -1;
    state->round = 0;
    state->started = 0;
    state->completed_seconds = 0;
    buf_clear(&state->request_id);
    code = 0;
done:
    state->restoring = false;
    buf_free(&state->history);
    buf_free(&state->history_warning);
    buf_free(&text);
    ui_destroy(&next);
    json_free(history);
    return code;
}
int ui_apply(UiState *state, const JVal *event, CaError *error) {
    const char *type = json_string(event, "type", "");
    if (!strcmp(type, "hello") || !strcmp(type, "workspace_ready") ||
        !strcmp(type, "session_ready"))
        return metadata(state, event, error);
    if (!strcmp(type, "session_history")) {
        JVal *text = json_get(event, "text");
        if (!state->restoring || !json_is_string(text) ||
            strcmp(json_string(event, "session_id", ""), buf_text(&state->session)))
            return ca_error(error, CA_ERROR, "无效会话历史响应");
        if (buf_append(&state->history, json_str(text), json_len(text), error) < 0)
            return -1;
        return json_bool(event, "done", false) ? restore_history(state, error) : 0;
    }
    if (!strcmp(type, "session_list"))
        return session_list(state, event, error);
    if (!strcmp(type, "context_usage")) {
        state->tokens =
            (int)json_int(event, "context_tokens", json_int(event, "input_tokens", state->tokens));
        state->context = (int)json_int(event, "context", state->context);
        state->input_limit = (int)json_int(event, "input_limit", state->input_limit);
        int round = (int)json_int(event, "round", state->round);
        if (round != state->round) {
            state->round = round;
            return set_text(&state->status, "思考中", error);
        }
    } else if (!strcmp(type, "text_delta")) {
        if (set_text(&state->status, "生成中", error) < 0)
            return -1;
        if (!state->count || state->entries[state->count - 1].kind != UI_ASSISTANT)
            if (ui_add(state, UI_ASSISTANT, "", 0, error) < 0)
                return -1;
        JVal *text = json_get(event, "text");
        return buf_append(&state->entries[state->count - 1].text, json_str(text), json_len(text),
                          error);
    } else if (!strcmp(type, "tool_start"))
        return start_tool(state, event, error);
    else if (!strcmp(type, "tool_output") && state->active_tool >= 0) {
        Buffer *output = &state->entries[state->active_tool].output;
        if (output->len < 128 * 1024) {
            JVal *text = json_get(event, "text");
            size_t room = 128 * 1024 - output->len;
            size_t n = json_len(text) < room ? json_len(text) : room;
            n = utf8_prefix_length(json_str(text), n, false);
            if (buf_append(output, json_str(text), n, error) < 0)
                return -1;
            if (json_len(text) >= room) {
                const char *marker = "\n[界面内容已截断，完整输出保存在命令日志]\n";
                return buf_append(output, marker, strlen(marker), error);
            }
        }
    } else if (!strcmp(type, "tool_result"))
        return tool_result(state, event, error);
    else if (!strcmp(type, "error") || !strcmp(type, "workspace_error") ||
             !strcmp(type, "session_error") || !strcmp(type, "notice")) {
        if (!strcmp(type, "session_error")) {
            state->restoring = false;
            buf_free(&state->history);
            buf_free(&state->history_warning);
        }
        return notice(state, json_string(event, "message", "未知错误"), error);
    } else if (!strcmp(type, "task_done"))
        return task_done(state, event, error);
    return 0;
}
void ui_toggle(UiState *state, int index) {
    if (index >= 0 && (size_t)index < state->count && state->entries[index].kind == UI_TOOL)
        state->entries[index].expanded = !state->entries[index].expanded;
}
int ui_summary(const UiState *state, int index, Buffer *text, CaError *error) {
    if (index < 0 || (size_t)index >= state->count)
        return ca_error(error, CA_ERROR, "无效界面条目");
    const UiEntry *entry = &state->entries[index];
    JVal *args = JV(entry->args), *result = JV(entry->result);
    if (buf_printf(text, error, "%s%s", entry->expanded ? "▾ " : "▸ ", buf_text(&entry->name)) < 0)
        return -1;
    const char *path = json_string(args, "file_path", json_string(args, "path", ""));
    if (*path && buf_printf(text, error, "  %s", path) < 0)
        return -1;
    if (!strcmp(buf_text(&entry->name), "Glob") &&
        buf_printf(text, error, "  %s", json_string(args, "pattern", "")) < 0)
        return -1;
    if (!strcmp(buf_text(&entry->name), "Bash")) {
        const char *description =
            json_string(args, "description", json_string(args, "command", ""));
        size_t n = strlen(description),
               length = n > 120 ? utf8_prefix_length(description, 120, false) : n;
        if (buf_append(text, "  ", 2, error) < 0)
            return -1;
        size_t begin = text->len;
        if (buf_append(text, description, length, error) < 0 ||
            (n > 120 && buf_append(text, "…", strlen("…"), error) < 0))
            return -1;
        for (size_t i = begin; i < text->len; ++i)
            if (text->data[i] == '\n')
                text->data[i] = ' ';
        if (json_get(result, "exit_code") &&
            buf_printf(text, error, "  exit=%lld", (long long)json_int(result, "exit_code", -1)) <
                0)
            return -1;
    }
    const char *status = entry->running                   ? "执行中"
                         : !entry->result                 ? "已中止"
                         : json_bool(result, "ok", false) ? "完成"
                                                          : "失败";
    if (buf_printf(text, error, "  · %s", status) < 0)
        return -1;
    if (entry->running || !entry->result || !json_bool(result, "ok", false))
        return 0;
    if (json_get(result, "files") &&
        buf_printf(text, error, "  %zu 个文件", json_len(json_get(result, "files"))) < 0)
        return -1;
    if (json_get(result, "bytes") &&
        buf_printf(text, error, "  %lld B", (long long)json_int(result, "bytes", 0)) < 0)
        return -1;
    if (!strcmp(buf_text(&entry->name), "Read") &&
        buf_printf(text, error, "  %zu B", json_len(json_get(result, "content"))) < 0)
        return -1;
    if (json_get(result, "replacements") &&
        buf_printf(text, error, "  %lld 处替换", (long long)json_int(result, "replacements", 0)) <
            0)
        return -1;
    return 0;
}
int ui_details(const UiState *state, int index, Buffer *text, CaError *error) {
    if (index < 0 || (size_t)index >= state->count)
        return ca_error(error, CA_ERROR, "无效界面条目");
    const UiEntry *entry = &state->entries[index];
    char *args = entry->args ? json_dump(entry->args, true, NULL) : ca_strdup("{}", error);
    if (!args)
        return ca_error(error, CA_NOMEM, "JSON 序列化失败");
    int code = buf_printf(text, error, "参数 JSON\n%s", args);
    free(args);
    if (code < 0)
        return -1;
    JVal *content = json_get(JV(entry->args), "content");
    if (json_is_string(content)) {
        if (buf_append(text, "\n文件内容\n", strlen("\n文件内容\n"), error) < 0 ||
            buf_append(text, json_str(content), json_len(content), error) < 0)
            return -1;
    }
    if (entry->output.len && (buf_append(text, "\n命令输出\n", strlen("\n命令输出\n"), error) < 0 ||
                              buf_append(text, entry->output.data, entry->output.len, error) < 0))
        return -1;
    if (entry->result) {
        char *result = json_dump(entry->result, true, NULL);
        if (!result)
            return ca_error(error, CA_NOMEM, "JSON 序列化失败");
        code = buf_printf(text, error, "\n结果 JSON\n%s", result);
        free(result);
    }
    return code;
}
