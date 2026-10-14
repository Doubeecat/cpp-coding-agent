#include "client_ui.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int socket, sequence;
    bool json, verbose, tui;
    UiState ui;
    Buffer pending;
} Client;
static void help(void) {
    puts("agent-client [--host 127.0.0.1] [--port 8888] [--workdir DIR] [--resume ID]\n"
         "             [--plain | --verbose | --json] [--prompt TEXT ...]\n"
         "交互终端默认使用 TUI；--workdir 直接读写指定目录；--resume 恢复已有会话。\n"
         "--plain 简略文本；--verbose 完整工具内容；--json 完整 JSON 事件。\n"
         "交互命令：/sessions 列出会话；/resume ID 恢复会话；/dir [路径] 查看或切换工作目录。");
}
static int print_fragment(Client *client, const JVal *event, bool final, CaError *error) {
    JVal *text = json_get(event, "text");
    if (text && buf_append(&client->pending, json_str(text), json_len(text), error) < 0)
        return -1;
    size_t length = utf8_prefix_length(buf_text(&client->pending), client->pending.len, final);
    if (length && fwrite(client->pending.data, 1, length, stdout) != length)
        return ca_errno(error, "写终端");
    buf_consume(&client->pending, length);
    return 0;
}
static int print_history(Client *client, CaError *error) {
    for (size_t i = 0; i < client->ui.count; ++i) {
        const UiEntry *entry = &client->ui.entries[i];
        if (entry->kind == UI_TOOL) {
            Buffer text = {0};
            int code = ui_summary(&client->ui, (int)i, &text, error);
            if (!code && client->verbose)
                code = buf_append(&text, "\n", 1, error);
            if (!code && client->verbose)
                code = ui_details(&client->ui, (int)i, &text, error);
            if (!code)
                printf("\n%s\n", buf_text(&text));
            buf_free(&text);
            if (code < 0)
                return -1;
        } else {
            const char *label = entry->kind == UI_USER        ? "你> "
                                : entry->kind == UI_ASSISTANT ? "Agent> "
                                                              : "";
            printf("\n%s", label);
            if (entry->text.len &&
                fwrite(entry->text.data, 1, entry->text.len, stdout) != entry->text.len)
                return ca_errno(error, "写终端");
            putchar('\n');
        }
    }
    return 0;
}
static int display(Client *client, const JVal *event, CaError *error) {
    if (ui_apply(&client->ui, event, error) < 0)
        return -1;
    if (client->tui)
        return 0;
    if (client->json) {
        char *text = json_dump_value(event, false, NULL);
        if (!text)
            return ca_error(error, CA_NOMEM, "JSON 序列化失败");
        puts(text);
        free(text);
        return fflush(stdout) == 0 ? 0 : ca_errno(error, "写终端");
    }
    const char *type = json_string(event, "type", "");
    if (!strcmp(type, "hello")) {
        printf("已连接，会话 %s\n代码目录：%s\n模型：%s · 上下文上限：%d tokens\n输入 /quit 退出\n",
               buf_text(&client->ui.session), buf_text(&client->ui.workspace),
               buf_text(&client->ui.model), client->ui.context);
    } else if (!strcmp(type, "session_history") && json_bool(event, "done", false)) {
        if (print_history(client, error) < 0)
            return -1;
    } else if (!strcmp(type, "text_delta") || (!strcmp(type, "tool_output") && client->verbose)) {
        if (print_fragment(client, event, false, error) < 0)
            return -1;
    } else if (!strcmp(type, "tool_start") || !strcmp(type, "tool_result")) {
        Buffer summary = {0};
        int code = ui_summary(&client->ui, (int)client->ui.count - 1, &summary, error);
        if (!code)
            printf("\n%s\n", buf_text(&summary));
        buf_free(&summary);
        if (code < 0)
            return -1;
        if (client->verbose) {
            JVal *value = json_get(event, !strcmp(type, "tool_start") ? "args" : "result");
            char *text = json_dump_value(value, true, NULL);
            if (!text)
                return ca_error(error, CA_NOMEM, "JSON 序列化失败");
            puts(text);
            free(text);
        }
    } else if (!strcmp(type, "error") || !strcmp(type, "workspace_error") ||
               !strcmp(type, "session_error")) {
        printf("\n[错误] %s\n", json_string(event, "message", ""));
    } else if (!strcmp(type, "notice"))
        printf("\n%s\n", json_string(event, "message", ""));
    else if (!strcmp(type, "task_done")) {
        if (print_fragment(client, NULL, true, error) < 0)
            return -1;
        printf("\n[本轮 %s]\n", json_string(event, "status", "failed"));
    }
    return fflush(stdout) == 0 ? 0 : ca_errno(error, "写终端");
}
static Json *control_request(const char *type, const char *key, const char *value) {
    Json *request = json_event(type);
    if (!strcmp(type, "resume_session"))
        json_set_bool(request, "include_history", true);
    if (key)
        json_set_string(request, key, value);
    return request;
}
static int exchange(Client *client, Json *request, Json **reply, CaError *error) {
    const char *type = json_string(JV(request), "type", "");
    int code = -1;
    if (!request || request->failed) {
        ca_error(error, CA_NOMEM, "内存不足");
        goto done;
    }
    if (send_frame(client->socket, JV(request), 10000, error) < 0)
        goto done;
    if (!strcmp(type, "resume_session"))
        client->ui.restoring = true;
    if (receive_frame(client->socket, reply, 3600000, error) != 1) {
        if (!error->code)
            ca_error(error, CA_ERROR, "服务端在响应会话请求时断开");
        goto done;
    }
    if (display(client, JV(*reply), error) < 0)
        goto done;
    while (client->ui.restoring) {
        Json *event = NULL;
        if (receive_frame(client->socket, &event, 3600000, error) != 1) {
            if (!error->code)
                ca_error(error, CA_ERROR, "服务端在恢复历史时断开");
            goto done;
        }
        bool history = !strcmp(json_string(JV(event), "type", ""), "session_history");
        int status = display(client, JV(event), error);
        json_free(event);
        if (status < 0)
            goto done;
        if (!history) {
            ca_error(error, CA_ERROR, "会话历史传输中断");
            goto done;
        }
    }
    if (!client->json && !client->tui && !strcmp(type, "list_sessions") &&
        !strcmp(json_string(JV(*reply), "type", ""), "session_list")) {
        JVal *sessions = json_get(JV(*reply), "sessions");
        printf("共 %zu 个会话：\n", json_len(sessions));
        for (size_t i = 0; i < json_len(sessions); ++i) {
            JVal *item = json_at(sessions, i);
            const char *id = json_string(item, "session_id", "");
            printf("%s%s  %lld 条  %s  %s\n",
                   !strcmp(id, json_string(JV(*reply), "current", "")) ? "  * " : "    ", id,
                   (long long)json_int(item, "messages", 0), json_string(item, "modified", ""),
                   json_string(item, "workspace", ""));
        }
        fflush(stdout);
    } else if (!client->json && !client->tui) {
        const char *result = json_string(JV(*reply), "type", "");
        if (!strcmp(result, "session_ready") || !strcmp(result, "workspace_ready")) {
            printf("\n当前会话 %s，目录 %s\n", buf_text(&client->ui.session),
                   buf_text(&client->ui.workspace));
            fflush(stdout);
        }
    }
    code = 0;
done:
    json_free(request);
    return code;
}
static int ask(Client *client, const char *text, CaError *error) {
    char id[24];
    snprintf(id, sizeof(id), "%d", ++client->sequence);
    if (ui_ask(&client->ui, text, strlen(text), id, error) < 0)
        return -1;
    Json *request = json_event("user_message");
    json_set_string(request, "request_id", id);
    json_set_string(request, "text", text);
    int code = request && !request->failed ? send_frame(client->socket, JV(request), 10000, error)
                                           : ca_error(error, CA_NOMEM, "内存不足");
    json_free(request);
    if (code < 0)
        return -1;
    for (;;) {
        Json *event = NULL;
        if (receive_frame(client->socket, &event, 3600000, error) != 1) {
            if (!error->code)
                ca_error(error, CA_ERROR, "服务端在任务结束前断开");
            return -1;
        }
        if (strcmp(json_string(JV(event), "request_id", ""), id)) {
            json_free(event);
            return ca_error(error, CA_ERROR, "服务端返回错误的 request_id");
        }
        bool done = !strcmp(json_string(JV(event), "type", ""), "task_done");
        code = display(client, JV(event), error);
        json_free(event);
        if (code < 0)
            return -1;
        if (done)
            return 0;
    }
}
static int interactive(Client *client, CaError *error) {
    char *line = NULL;
    size_t capacity = 0;
    int code = 0;
    while (!stopping) {
        if (!client->json && isatty(STDIN_FILENO)) {
            fputs("\n你> ", stdout);
            fflush(stdout);
        }
        ssize_t length = getline(&line, &capacity, stdin);
        if (length < 0)
            break;
        if (length && line[length - 1] == '\n')
            line[--length] = 0;
        if (!strcmp(line, "/quit"))
            break;
        if (!length)
            continue;
        char *space = strchr(line, ' '), *argument = space ? space + 1 : line + length;
        while (*argument == ' ')
            ++argument;
        if (space)
            *space = 0;
        Json *request = NULL, *reply = NULL;
        if (!strcmp(line, "/sessions"))
            request = control_request("list_sessions", NULL, NULL);
        else if (!strcmp(line, "/resume")) {
            if (!*argument)
                puts("用法：/resume <会话 ID>，先用 /sessions 查看");
            else
                request = control_request("resume_session", "session_id", argument);
        } else if (!strcmp(line, "/dir")) {
            if (!*argument)
                printf("当前目录：%s\n", buf_text(&client->ui.workspace));
            else {
                char *joined = *argument == '/'
                                   ? ca_strdup(argument, error)
                                   : path_join(buf_text(&client->ui.workspace), argument, error);
                char *path = joined ? path_absolute(joined, error) : NULL;
                free(joined);
                if (!path) {
                    code = -1;
                    break;
                }
                request = control_request("set_workspace", "workspace", path);
                free(path);
            }
        } else {
            if (space)
                *space = ' ';
            code = ask(client, line, error);
            if (code < 0)
                break;
        }
        if (request) {
            code = exchange(client, request, &reply, error);
            json_free(reply);
            if (code < 0)
                break;
        }
    }
    free(line);
    return code;
}
int main(int argc, char **argv) {
    const char *host = "127.0.0.1", *resume = NULL;
    char *workdir = NULL;
    int port = 8888, code = 1;
    bool plain = false;
    const char **prompts = NULL;
    size_t count = 0;
    Client client = {.socket = -1};
    Json *hello = NULL, *reply = NULL;
    CaError error = {0};
    if (ui_init(&client.ui, &error) < 0)
        goto done;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) {
            help();
            code = 0;
            goto done;
        }
        if (!strcmp(key, "--json")) {
            client.json = true;
            continue;
        }
        if (!strcmp(key, "--plain") || !strcmp(key, "--verbose")) {
            plain = true;
            if (!strcmp(key, "--verbose"))
                client.verbose = true;
            continue;
        }
        if (++i == argc) {
            ca_error(&error, CA_ERROR, "参数缺少值：%s", key);
            goto done;
        }
        const char *value = argv[i];
        if (!strcmp(key, "--host"))
            host = value;
        else if (!strcmp(key, "--port")) {
            if (parse_int_arg(value, &port, &error) < 0)
                goto done;
        } else if (!strcmp(key, "--resume")) {
            if (!*value) {
                ca_error(&error, CA_ERROR, "会话 ID 不能为空");
                goto done;
            }
            resume = value;
        } else if (!strcmp(key, "--workdir")) {
            free(workdir);
            workdir = path_absolute(value, &error);
            if (!workdir)
                goto done;
        } else if (!strcmp(key, "--prompt")) {
            const char **next = realloc(prompts, (count + 1) * sizeof(*next));
            if (!next) {
                ca_error(&error, CA_NOMEM, "内存不足");
                goto done;
            }
            prompts = next;
            prompts[count++] = value;
        } else {
            ca_error(&error, CA_ERROR, "未知参数：%s", key);
            goto done;
        }
    }
    if (port < 1 || port > 65535) {
        ca_error(&error, CA_ERROR, "端口范围非法");
        goto done;
    }
    const char *term = getenv("TERM");
    client.tui = !client.json && !plain && !count && isatty(STDIN_FILENO) &&
                 isatty(STDOUT_FILENO) && term && *term && strcmp(term, "dumb");
    install_signals();
    client.socket = connect_tcp(host, port, &error);
    if (client.socket < 0)
        goto done;
    if (receive_frame(client.socket, &hello, 3600000, &error) != 1) {
        if (!error.code)
            ca_error(&error, CA_ERROR, "服务端未返回欢迎消息");
        goto done;
    }
    if (strcmp(json_string(JV(hello), "type", ""), "hello")) {
        display(&client, JV(hello), &error);
        goto done;
    }
    if (display(&client, JV(hello), &error) < 0)
        goto done;
    if (resume) {
        if (exchange(&client, control_request("resume_session", "session_id", resume), &reply,
                     &error) < 0 ||
            strcmp(json_string(JV(reply), "type", ""), "session_ready"))
            goto done;
        json_free(reply);
        reply = NULL;
    }
    if (workdir) {
        if (exchange(&client, control_request("set_workspace", "workspace", workdir), &reply,
                     &error) < 0 ||
            strcmp(json_string(JV(reply), "type", ""), "workspace_ready"))
            goto done;
        json_free(reply);
        reply = NULL;
    }
    if (client.tui) {
        code = run_tui(client.socket, &client.ui, &error);
        goto done;
    }
    if (count) {
        for (size_t i = 0; i < count; ++i)
            if (ask(&client, prompts[i], &error) < 0)
                goto done;
    } else if (interactive(&client, &error) < 0)
        goto done;
    if (!stopping) {
        Json *quit = json_event("quit");
        CaError ignored = {0};
        send_frame(client.socket, JV(quit), 1000, &ignored);
        json_free(quit);
    }
    code = client.ui.exit_code;
done:
    if (error.code) {
        fprintf(stderr, "client: %s\n", error.message);
        code = 1;
    }
    close_fd(&client.socket);
    json_free(hello);
    json_free(reply);
    ui_destroy(&client.ui);
    buf_free(&client.pending);
    free(workdir);
    free(prompts);
    return code;
}
