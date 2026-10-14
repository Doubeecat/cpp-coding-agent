#include "common.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int socket, sequence;
    Json *hello;
    FILE *log;
    Buffer answer;
} Client;
static void client_close(Client *client) {
    close_fd(&client->socket);
    json_free(client->hello);
    if (client->log)
        fclose(client->log);
    buf_free(&client->answer);
}
static int client_open(Client *client, int port, const char *path, CaError *error) {
    client->socket = connect_tcp("127.0.0.1", port, error);
    if (client->socket < 0)
        return -1;
    client->log = fopen(path, "w");
    if (!client->log)
        return ca_errno(error, "创建验收日志");
    if (receive_frame(client->socket, &client->hello, 10000, error) != 1 ||
        strcmp(json_string(JV(client->hello), "type", ""), "hello"))
        return ca_error(error, CA_ERROR, "缺少 hello");
    return append_json(client->log, JV(client->hello), error);
}
static int ask(Client *client, const char *text, CaError *error) {
    buf_clear(&client->answer);
    char id[24];
    snprintf(id, sizeof(id), "%d", ++client->sequence);
    Json *request = json_event("user_message");
    json_set_string(request, "request_id", id);
    json_set_string(request, "text", text);
    int code = request && !request->failed ? send_frame(client->socket, JV(request), 10000, error)
                                           : ca_error(error, CA_NOMEM, "内存不足");
    json_free(request);
    return code;
}
static int receive_event(Client *client, Json **event, CaError *error) {
    char id[24];
    snprintf(id, sizeof(id), "%d", client->sequence);
    if (receive_frame(client->socket, event, 150000, error) != 1)
        return ca_error(error, CA_ERROR, "连接提前关闭");
    if (strcmp(json_string(JV(*event), "request_id", ""), id))
        return ca_error(error, CA_ERROR, "request_id 错误");
    if (append_json(client->log, JV(*event), error) < 0)
        return -1;
    if (!strcmp(json_string(JV(*event), "type", ""), "text_delta")) {
        JVal *text = json_get(JV(*event), "text");
        return buf_append(&client->answer, json_str(text), json_len(text), error);
    }
    return 0;
}
static int collect_pair(Client *a, Client *b, bool require_overlap, CaError *error) {
    Client *clients[] = {a, b};
    bool done[] = {false, false};
    int chunks[] = {0, 0};
    int64_t deadline = monotonic_ms() + 170000;
    while (!done[0] || !done[1]) {
        if (monotonic_ms() >= deadline || stopping)
            return ca_error(error, CA_ERROR, "并发任务超时或中止");
        struct pollfd fds[] = {{done[0] ? -1 : a->socket, POLLIN, 0},
                               {done[1] ? -1 : b->socket, POLLIN, 0}};
        if (poll(fds, 2, 200) <= 0)
            continue;
        for (int i = 0; i < 2; ++i)
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                Json *event = NULL;
                int code = receive_event(clients[i], &event, error);
                if (!code) {
                    const char *type = json_string(JV(event), "type", "");
                    if (!strcmp(type, "text_delta"))
                        ++chunks[i];
                    if (!strcmp(type, "task_done")) {
                        if (strcmp(json_string(JV(event), "status", ""), "success"))
                            code = ca_error(error, CA_ERROR, "真实模型未成功完成任务");
                        else if (require_overlap && (chunks[0] < 2 || chunks[1] < 2))
                            code = ca_error(error, CA_ERROR, "两个模型响应没有重叠流式输出");
                        done[i] = true;
                    }
                }
                json_free(event);
                if (code < 0)
                    return -1;
            }
    }
    return 0;
}
static int disconnect_check(Client *a, Client *b, Json *report, CaError *error) {
    if (ask(a, "请用不少于600字介绍 socket、bind、listen、accept。直接回答，不修改文件。", error) <
            0 ||
        ask(b, "请用约300字介绍 fork 如何让父子进程同时工作。直接回答，不修改文件。", error) < 0)
        return -1;
    Client *clients[] = {a, b};
    bool started[] = {false, false};
    int64_t deadline = monotonic_ms() + 150000;
    while (!started[0] || !started[1]) {
        if (monotonic_ms() >= deadline || stopping)
            return ca_error(error, CA_ERROR, "断连测试未开始生成");
        struct pollfd fds[] = {{started[0] ? -1 : a->socket, POLLIN, 0},
                               {started[1] ? -1 : b->socket, POLLIN, 0}};
        if (poll(fds, 2, 200) <= 0)
            continue;
        for (int i = 0; i < 2; ++i)
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                Json *event = NULL;
                int code = receive_event(clients[i], &event, error);
                if (!code && !strcmp(json_string(JV(event), "type", ""), "task_done"))
                    code = ca_error(error, CA_ERROR, "关闭 A 前任务已结束");
                if (!code && !strcmp(json_string(JV(event), "type", ""), "text_delta"))
                    started[i] = true;
                json_free(event);
                if (code < 0)
                    return -1;
            }
    }
    pid_t pid = (pid_t)json_int(JV(a->hello), "pid", 0);
    close_fd(&a->socket);
    int chunks = 0;
    for (;;) {
        Json *event = NULL;
        int code = receive_event(b, &event, error);
        bool done = false;
        if (!code) {
            if (!strcmp(json_string(JV(event), "type", ""), "text_delta"))
                ++chunks;
            done = !strcmp(json_string(JV(event), "type", ""), "task_done");
            if (done && (strcmp(json_string(JV(event), "status", ""), "success") || !chunks))
                code = ca_error(error, CA_ERROR, "A 断开后 B 未继续输出");
        }
        json_free(event);
        if (code < 0)
            return -1;
        if (done)
            break;
    }
    bool reaped = false;
    for (int i = 0; i < 150; ++i) {
        if (kill(pid, 0) < 0 && errno == ESRCH) {
            reaped = true;
            break;
        }
        sleep_ms(20);
    }
    if (!reaped)
        return ca_error(error, CA_ERROR, "A 会话进程未回收");
    json_set_int(report, "b_chunks_after_disconnect", chunks);
    json_set_bool(report, "disconnected_process_reaped", true);
    puts("PASS 关闭 A 后 B 继续生成，A 会话已回收");
    return 0;
}
int main(int argc, char **argv) {
    CaError error = {0};
    int port = 8888, code = 1;
    Client a = {.socket = -1}, b = {.socket = -1};
    Json *report = json_object();
    Buffer prefix = {0};
    char *path_a = NULL, *path_b = NULL, *summary = NULL;
    char time[40];
    timestamp(time);
    json_set_string(report, "time", time);
    json_set_string(report, "backend", "local Qwen3.5-4B-Q8_0");
    if (argc > 1 && parse_int_arg(argv[1], &port, &error) < 0)
        goto done;
    if (port < 1 || port > 65535) {
        ca_error(&error, CA_ERROR, "端口范围非法");
        goto done;
    }
    if (mkdir_parents(AGENT_PROJECT_DIR "/runtime", 0700, &error) < 0 ||
        buf_printf(&prefix, &error, AGENT_PROJECT_DIR "/runtime/live-%ld", (long)getpid()) < 0)
        goto done;
    Buffer name = {0};
    if (buf_printf(&name, &error, "%s-a.jsonl", prefix.data) < 0) {
        buf_free(&name);
        goto done;
    }
    path_a = name.data;
    memset(&name, 0, sizeof(name));
    if (buf_printf(&name, &error, "%s-b.jsonl", prefix.data) < 0) {
        buf_free(&name);
        goto done;
    }
    path_b = name.data;
    memset(&name, 0, sizeof(name));
    if (buf_printf(&name, &error, "%s-summary.json", prefix.data) < 0) {
        buf_free(&name);
        goto done;
    }
    summary = name.data;
    install_signals();
    if (client_open(&a, port, path_a, &error) < 0 || client_open(&b, port, path_b, &error) < 0)
        goto done;
    if (!strcmp(json_string(JV(a.hello), "session_id", ""),
                json_string(JV(b.hello), "session_id", "")) ||
        json_int(JV(a.hello), "pid", 0) == json_int(JV(b.hello), "pid", 0)) {
        ca_error(&error, CA_ERROR, "会话进程没有隔离");
        goto done;
    }
    json_set_value(report, "session_a", json_get(JV(a.hello), "session_id"));
    json_set_value(report, "session_b", json_get(JV(b.hello), "session_id"));
    if (ask(&a,
            "请记住本连接项目代号 ALPHA_314159。先写出代号，再用约150字介绍 C "
            "的函数。只回答，不操作文件。",
            &error) < 0 ||
        ask(&b,
            "请记住本连接项目代号 BETA_271828。先写出代号，再用约150字介绍 C "
            "的数组。只回答，不操作文件。",
            &error) < 0 ||
        collect_pair(&a, &b, true, &error) < 0)
        goto done;
    json_set_bool(report, "overlapping_streams", true);
    puts("PASS 真实模型双客户端同时流式输出");
    if (ask(&a, "刚才让你记住的本连接项目代号是什么？只输出代号。", &error) < 0 ||
        ask(&b, "刚才让你记住的本连接项目代号是什么？只输出代号。", &error) < 0 ||
        collect_pair(&a, &b, false, &error) < 0)
        goto done;
    json_set_string(report, "recall_a", buf_text(&a.answer));
    json_set_string(report, "recall_b", buf_text(&b.answer));
    if (!strstr(buf_text(&a.answer), "ALPHA_314159") ||
        strstr(buf_text(&a.answer), "BETA_271828") || !strstr(buf_text(&b.answer), "BETA_271828") ||
        strstr(buf_text(&b.answer), "ALPHA_314159")) {
        ca_error(&error, CA_ERROR, "模型会话记忆串线");
        goto done;
    }
    puts("PASS 同连接连续提问与不同连接上下文隔离");
    if (disconnect_check(&a, &b, report, &error) < 0)
        goto done;
    code = 0;
done:
    json_set_string(report, "status", code ? "failed" : "passed");
    if (error.code) {
        json_set_string(report, "error", error.message);
        fprintf(stderr, "%s\n", error.message);
    }
    if (summary) {
        char *text = json_dump(report, true, NULL);
        CaError save_error = {0};
        if (!text || write_file_bytes(summary, text, text ? strlen(text) : 0, &save_error) < 0) {
            fprintf(stderr, "保存验收记录失败：%s\n", save_error.message);
            code = 1;
        }
        free(text);
        printf("验收记录：%s\n", summary);
    }
    client_close(&a);
    client_close(&b);
    json_free(report);
    buf_free(&prefix);
    free(path_a);
    free(path_b);
    free(summary);
    return code;
}
