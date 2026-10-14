#include "client_ui.h"
#include "test_support.h"
#include <sys/ioctl.h>
#include <termios.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

static void apply(UiState *state, const char *literal) {
    Json *event = json_literal(literal);
    CHECK(event, "UI event JSON");
    OK(ui_apply(state, JV(event), &error));
    json_free(event);
}
static void ui_state_tests(void) {
    UiState state;
    OK(ui_init(&state, &error));
    apply(&state, "{\"type\":\"hello\",\"workspace\":\"/tmp/"
                  "我的项目\",\"model\":\"qwen3.5-4b\",\"context\":32768}");
    CHECK(state.tokens == -1 && !strcmp(buf_text(&state.workspace), "/tmp/我的项目"),
          "metadata owned after event release, no invented tokens");
    OK(ui_ask(&state, "生成代码", strlen("生成代码"), "1", &error));
    sleep_ms(20);
    CHECK(state.busy && ui_elapsed(&state) >= 0.01 && !strcmp(buf_text(&state.status), "思考中"),
          "thinking timer starts immediately");
    apply(&state, "{\"type\":\"context_usage\",\"context_tokens\":1234,\"round\":1}");
    CHECK(state.tokens == 1234, "measured token count");
    apply(&state, "{\"type\":\"tool_start\",\"name\":\"Write\",\"args\":{\"file_path\":\"src/"
                  "main.c\",\"content\":\"CODE_MARKER\"}}");
    int write = (int)state.count - 1;
    Buffer text = {0};
    OK(ui_summary(&state, write, &text, &error));
    CHECK(strstr(text.data, "src/main.c") && !strstr(text.data, "CODE_MARKER") &&
              !state.entries[write].expanded,
          "tool source collapsed by default");
    ui_toggle(&state, write);
    buf_clear(&text);
    OK(ui_details(&state, write, &text, &error));
    CHECK(strstr(text.data, "CODE_MARKER"), "complete owned source available in expanded card");
    apply(&state,
          "{\"type\":\"tool_result\",\"name\":\"Write\",\"result\":{\"ok\":true,\"bytes\":11}}");
    apply(&state,
          "{\"type\":\"tool_start\",\"name\":\"Bash\",\"args\":{\"command\":\"make test\"}}");
    int command = (int)state.count - 1;
    apply(&state, "{\"type\":\"tool_output\",\"text\":\"BUILD_MARKER\"}");
    apply(&state, "{\"type\":\"tool_result\",\"name\":\"Bash\",\"result\":{\"ok\":false,\"exit_"
                  "code\":2,\"output\":\"error\",\"log_file\":\"command-1.log\"}}");
    buf_clear(&text);
    OK(ui_summary(&state, command, &text, &error));
    CHECK(strstr(text.data, "exit=2") && strstr(text.data, "失败"), "actual tool failure visible");
    buf_clear(&text);
    OK(ui_details(&state, command, &text, &error));
    CHECK(strstr(text.data, "BUILD_MARKER") && strstr(text.data, "command-1.log"),
          "streamed output and full result preserved");
    apply(&state,
          "{\"type\":\"tool_start\",\"name\":\"Read\",\"args\":{\"file_path\":\"src/main.c\"}}");
    apply(&state, "{\"type\":\"task_done\",\"status\":\"cancelled\"}");
    buf_clear(&text);
    OK(ui_summary(&state, (int)state.count - 1, &text, &error));
    CHECK(strstr(text.data, "已中止") && !state.busy, "interrupted tool stays renderable");
    double elapsed = ui_elapsed(&state);
    sleep_ms(20);
    CHECK(ui_elapsed(&state) == elapsed, "completed timer stops");
    apply(&state, "{\"type\":\"tool_result\",\"name\":\"Write\",\"result\":{\"ok\":false,\"error\":"
                  "\"invalid arguments\"}}");
    buf_clear(&text);
    OK(ui_details(&state, (int)state.count - 1, &text, &error));
    CHECK(strstr(text.data, "invalid arguments"), "result without tool_start represented");
    apply(&state, "{\"type\":\"session_list\",\"sessions\":[{\"session_id\":\"old\",\"workspace\":"
                  "null,\"modified\":null,\"messages\":3}],\"current\":\"old\"}");
    ui_destroy(&state);
    buf_free(&text);
    puts("PASS UI ownership, metadata, cards, partial tools and timers");
}
static Json *history_chunk(const char *id, const char *text, size_t length, bool done) {
    Json *event = json_event("session_history");
    json_set_string(event, "session_id", id);
    json_set_stringn(event, "text", text, length);
    json_set_bool(event, "done", done);
    return event;
}
static void ui_history_tests(void) {
    UiState state;
    OK(ui_init(&state, &error));
    OK(ui_ask(&state, "STALE_CONVERSATION", 18, "9", &error));
    state.tokens = 1234;
    Json *history = json_literal(
        "[{\"role\":\"system\",\"content\":\"HIDDEN_SYSTEM\"},"
        "{\"role\":\"user\",\"content\":\"旧对话 USER_RESTORE\"},"
        "{\"role\":\"assistant\",\"content\":\"ASSISTANT_RESTORE\",\"tool_calls\":["
        "{\"id\":\"same\",\"function\":{\"name\":\"Write\",\"arguments\":"
        "\"{\\\"file_path\\\":\\\"src/main.c\\\",\\\"content\\\":\\\"SOURCE_RESTORE\\\"}\"}},"
        "{\"id\":\"cmd\",\"function\":{\"name\":\"Bash\",\"arguments\":"
        "\"{\\\"command\\\":\\\"make test\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"cmd\",\"content\":"
        "\"{\\\"ok\\\":false,\\\"exit_code\\\":2,\\\"output\\\":\\\"BUILD_ERROR_RESTORE\\\"}\"},"
        "{\"role\":\"tool\",\"tool_call_id\":\"same\",\"content\":\"{\\\"ok\\\":true,\\\"bytes\\\":"
        "14}\"},"
        "{\"role\":\"assistant\",\"content\":\"ASSISTANT_FINAL\"},"
        "{\"role\":\"user\",\"content\":\"SECOND_TURN\"},"
        "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
        "{\"id\":\"same\",\"function\":{\"name\":\"Edit\",\"arguments\":\"{invalid\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"same\",\"content\":"
        "\"{\\\"ok\\\":false,\\\"error\\\":\\\"EDIT_FAILURE_RESTORE\\\"}\"},"
        "{\"role\":\"assistant\",\"content\":\"SECOND_FINAL\"}]");
    CHECK(history, "saved conversation fixture");
    size_t length;
    char *text = json_dump(history, false, &length);
    CHECK(text, "history serialized");
    json_free(history);
    apply(&state, "{\"type\":\"session_ready\",\"session_id\":\"123-456\",\"history\":true,"
                  "\"warning\":\"WORKSPACE_FALLBACK_WARNING\"}");
    size_t split = utf8_prefix_length(text, length / 2, false);
    Json *event = history_chunk("123-456", text, split, false);
    OK(ui_apply(&state, JV(event), &error));
    json_free(event);
    CHECK(state.restoring && state.count == 1 &&
              !strcmp(buf_text(&state.entries[0].text), "STALE_CONVERSATION"),
          "partial snapshot keeps previous view intact");
    event = history_chunk("123-456", text + split, length - split, true);
    OK(ui_apply(&state, JV(event), &error));
    json_free(event);
    free(text);
    CHECK(!state.restoring && !state.busy && state.tokens == -1 && state.active_tool == -1 &&
              state.count == 10 && state.entries[0].kind == UI_USER &&
              !strcmp(buf_text(&state.entries[0].text), "旧对话 USER_RESTORE") &&
              state.entries[1].kind == UI_ASSISTANT &&
              !strcmp(buf_text(&state.entries[7].text), "SECOND_FINAL"),
          "restored conversation replaces stale view and resets task state");
    CHECK(
        !state.entries[2].expanded && !state.entries[2].running &&
            json_bool(JV(state.entries[2].result), "ok", false) &&
            json_int(JV(state.entries[3].result), "exit_code", 0) == 2 &&
            !strcmp(json_string(JV(state.entries[6].result), "error", ""), "EDIT_FAILURE_RESTORE"),
        "tool results pair by ID within each assistant round");
    Buffer details = {0};
    ui_toggle(&state, 2);
    OK(ui_details(&state, 2, &details, &error));
    CHECK(strstr(buf_text(&details), "SOURCE_RESTORE"), "restored source remains expandable");
    CHECK(strstr(buf_text(&state.entries[9].text), "WORKSPACE_FALLBACK_WARNING"),
          "workspace fallback warning survives restored view");
    apply(&state, "{\"type\":\"session_ready\",\"session_id\":\"222-333\",\"history\":true}");
    event = history_chunk("222-333", "{bad", 4, true);
    CHECK(ui_apply(&state, JV(event), &error) < 0 && state.count == 10 && !state.restoring,
          "malformed snapshot does not erase existing conversation");
    ca_clear_error(&error);
    json_free(event);
    apply(&state, "{\"type\":\"session_ready\",\"session_id\":\"333-444\",\"history\":true}");
    event = history_chunk("333-444", "[]", 2, true);
    OK(ui_apply(&state, JV(event), &error));
    json_free(event);
    CHECK(state.count == 1 && state.entries[0].kind == UI_NOTICE,
          "empty session removes previous conversation and cards");
    state.restoring = true;
    apply(&state, "{\"type\":\"session_ready\",\"session_id\":\"444-555\",\"messages\":2}");
    CHECK(!state.restoring && strstr(buf_text(&state.entries[state.count - 1].text), "重启"),
          "older server without history prompts restart and does not leave UI blocked");
    buf_free(&details);
    ui_destroy(&state);
    puts("PASS restored UI history: chunking, ownership, tool pairing, atomic replacement and "
         "warnings");
}
typedef struct {
    int terminal, socket;
    pid_t pid;
    Buffer screen;
} Pty;
static void drain_terminal(Pty *pty);
static void terminal_write(Pty *pty, const char *bytes, size_t length) {
    size_t offset = 0;
    int64_t deadline = monotonic_ms() + 10000;
    while (offset < length) {
        ssize_t n = write(pty->terminal, bytes + offset, length - offset);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            CHECK(monotonic_ms() < deadline, "PTY input timeout");
            drain_terminal(pty);
            struct pollfd wait = {pty->terminal, POLLIN | POLLOUT, 0};
            poll(&wait, 1, 30);
            continue;
        }
        CHECK(n > 0, "write PTY input");
        offset += (size_t)n;
    }
}
static void keys(Pty *pty, const char *text) {
    terminal_write(pty, text, strlen(text));
}
static void drain_terminal(Pty *pty) {
    for (;;) {
        char bytes[8192];
        ssize_t n = read(pty->terminal, bytes, sizeof(bytes));
        if (n <= 0)
            break;
        OK(buf_append(&pty->screen, bytes, (size_t)n, &error));
        CHECK(pty->screen.len <= 16 * CA_FRAME_LIMIT, "PTY output bounded");
    }
}
static void wait_text(Pty *pty, const char *text) {
    int64_t deadline = monotonic_ms() + 5000;
    while (!strstr(buf_text(&pty->screen), text) && monotonic_ms() < deadline) {
        struct pollfd wait = {pty->terminal, POLLIN, 0};
        poll(&wait, 1, 30);
        drain_terminal(pty);
    }
    if (!strstr(buf_text(&pty->screen), text)) {
        char *path = test_path("terminal-output.txt");
        write_file_bytes(path, buf_text(&pty->screen), pty->screen.len, &error);
        free(path);
        CHECK(false, text);
    }
}
static Json *client_frame(Pty *pty) {
    int64_t deadline = monotonic_ms() + 5000;
    for (;;) {
        struct pollfd wait[] = {{pty->terminal, POLLIN, 0}, {pty->socket, POLLIN, 0}};
        CHECK(monotonic_ms() < deadline, "client submits request while terminal remains drained");
        poll(wait, 2, 30);
        drain_terminal(pty);
        if (wait[1].revents & POLLIN)
            break;
    }
    Json *message = NULL;
    CHECK(receive_frame(pty->socket, &message, 5000, &error) == 1, "client request received");
    return message;
}
static void server_event(Pty *pty, Json *event, const char *id) {
    if (id)
        json_set_string(event, "request_id", id);
    OK(send_frame(pty->socket, JV(event), 5000, &error));
    json_free(event);
}
static void simple_event(Pty *pty, const char *literal, const char *id) {
    server_event(pty, json_literal(literal), id);
}
static void wheel(Pty *pty, bool down, int count) {
    for (int i = 0; i < count; ++i)
        keys(pty, down ? "\x1b[<65;5;5M" : "\x1b[<64;5;5M");
}
static Pty start_terminal(const char *binary, const char *option, const char *argument) {
    int listener = listen_tcp("127.0.0.1", 0, &error);
    CHECK(listener >= 0, "PTY listener");
    int port = socket_port(listener, &error);
    char number[16];
    snprintf(number, sizeof(number), "%d", port);
    struct winsize size = {.ws_row = 34, .ws_col = 112};
    Pty pty = {.terminal = -1, .socket = -1};
    pty.pid = forkpty(&pty.terminal, NULL, NULL, &size);
    CHECK(pty.pid >= 0, "forkpty");
    if (!pty.pid) {
        setenv("TERM", "xterm-256color", 1);
        setenv("LC_ALL", "en_US.UTF-8", 1);
        execl(binary, binary, "--port", number, option, argument, (char *)NULL);
        _exit(127);
    }
    register_process(pty.pid);
    OK(set_fd_flags(pty.terminal, true, &error));
    struct pollfd wait = {listener, POLLIN, 0};
    CHECK(poll(&wait, 1, 5000) > 0, "PTY client connects");
    pty.socket = accept(listener, NULL, NULL);
    close(listener);
    CHECK(pty.socket >= 0, "accept PTY client");
    OK(set_fd_flags(pty.socket, true, &error));
    Json *hello = json_event("hello");
    json_set_string(hello, "session_id", "123-456");
    json_set_string(hello, "workspace", test_root);
    json_set_string(hello, "model", "qwen3.5-4b");
    json_set_int(hello, "context", 32768);
    json_set_int(hello, "input_limit", 24576);
    server_event(&pty, hello, NULL);
    return pty;
}
static void stop_terminal(Pty *pty, int expected_exit) {
    keys(pty, "\x03");
    int status = 0;
    pid_t ended = 0;
    int64_t deadline = monotonic_ms() + 5000;
    while ((ended = waitpid(pty->pid, &status, WNOHANG)) == 0 && monotonic_ms() < deadline) {
        drain_terminal(pty);
        sleep_ms(20);
    }
    drain_terminal(pty);
    CHECK(ended == pty->pid, "PTY client terminated");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected_exit, "client exit status");
    CHECK(strstr(buf_text(&pty->screen), "\x1b[?2004l") &&
              strstr(buf_text(&pty->screen), "\x1b[?1006l"),
          "terminal paste and mouse modes restored");
    close_fd(&pty->terminal);
    close_fd(&pty->socket);
    buf_free(&pty->screen);
}
static void resume_terminal_tests(const char *binary) {
    Pty pty = start_terminal(binary, "--resume", "111-222");
    Json *request = client_frame(&pty);
    CHECK(!strcmp(json_string(JV(request), "type", ""), "resume_session") &&
              !strcmp(json_string(JV(request), "session_id", ""), "111-222") &&
              json_bool(JV(request), "include_history", false),
          "--resume requests visible history before TUI");
    json_free(request);
    simple_event(&pty, "{\"type\":\"session_ready\",\"session_id\":\"111-222\",\"history\":true}",
                 NULL);
    const char *history =
        "[{\"role\":\"user\",\"content\":\"STARTUP_USER_RESTORED\"},"
        "{\"role\":\"assistant\",\"content\":\"STARTUP_ASSISTANT_RESTORED\",\"tool_calls\":["
        "{\"id\":\"saved\",\"function\":{\"name\":\"Write\",\"arguments\":"
        "\"{\\\"file_path\\\":\\\"src/main.c\\\",\\\"content\\\":\\\"SAVED_SOURCE_CARD\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"saved\",\"content\":\"{\\\"ok\\\":true,"
        "\\\"bytes\\\":17}\"}]";
    server_event(&pty, history_chunk("111-222", history, 20, false), NULL);
    server_event(&pty, history_chunk("111-222", history + 20, strlen(history) - 20, true), NULL);
    wait_text(&pty, "STARTUP_USER_RESTORED");
    wait_text(&pty, "STARTUP_ASSISTANT_RESTORED");
    wait_text(&pty, "Write");
    CHECK(!strstr(buf_text(&pty.screen), "SAVED_SOURCE_CARD"), "restored cards start collapsed");
    keys(&pty, "\t\r");
    wait_text(&pty, "SAVED_SOURCE_CARD");
    keys(&pty, "\x1b");
    sleep_ms(40);
    keys(&pty, "/resume 222-333\r");
    request = client_frame(&pty);
    CHECK(!strcmp(json_string(JV(request), "type", ""), "resume_session") &&
              json_bool(JV(request), "include_history", false),
          "interactive /resume requests visible history");
    json_free(request);
    simple_event(&pty, "{\"type\":\"session_ready\",\"session_id\":\"222-333\",\"history\":true}",
                 NULL);
    history = "[{\"role\":\"user\",\"content\":\"切换后的用户消息\"},"
              "{\"role\":\"assistant\",\"content\":\"新会话的历史回复\"}]";
    server_event(&pty, history_chunk("222-333", history, 20, false), NULL);
    wait_text(&pty, "正在恢复");
    keys(&pty, "QUESTION_AFTER_RESUME\r");
    wait_text(&pty, "完成后再发送");
    struct pollfd no_request = {pty.socket, POLLIN, 0};
    CHECK(poll(&no_request, 1, 100) == 0, "restoring blocks submission while keeping draft");
    server_event(&pty, history_chunk("222-333", history + 20, strlen(history) - 20, true), NULL);
    wait_text(&pty, "切换后的用户消息");
    wait_text(&pty, "新会话的历史回复");
    keys(&pty, "\r");
    request = client_frame(&pty);
    CHECK(!strcmp(json_string(JV(request), "text", ""), "QUESTION_AFTER_RESUME"),
          "next prompt stays intact after session switch");
    json_free(request);
    simple_event(&pty, "{\"type\":\"text_delta\",\"text\":\"CONTINUED_AFTER_RESUME\"}", "1");
    simple_event(&pty, "{\"type\":\"task_done\",\"status\":\"success\"}", "1");
    wait_text(&pty, "CONTINUED_AFTER_RESUME");
    stop_terminal(&pty, 0);
    puts("PASS real PTY --resume and /resume: old chat, expandable cards and continued prompts");
}
static void terminal_tests(const char *binary) {
    char *directory = test_path("我的项目");
    Pty pty = start_terminal(binary, "--workdir", directory);
    struct winsize size;
    Json *configure = client_frame(&pty);
    CHECK(!strcmp(json_string(JV(configure), "type", ""), "set_workspace") &&
              !strcmp(json_string(JV(configure), "workspace", ""), directory),
          "--workdir request before TUI");
    json_free(configure);
    Json *ready = json_event("workspace_ready");
    json_set_string(ready, "session_id", "123-456");
    json_set_string(ready, "workspace", directory);
    json_set_string(ready, "model", "qwen3.5-4b");
    server_event(&pty, ready, NULL);
    wait_text(&pty, "Token");
    wait_text(&pty, "qwen3.5-4b");
    wait_text(&pty, "32768");
    wait_text(&pty, directory);
    keys(&pty, "\x1b[200~PROMPT_BEGIN\nsecond line\nPROMPT_END\x1b[201~");
    wait_text(&pty, "PROMPT_END");
    struct pollfd no_request = {pty.socket, POLLIN, 0};
    CHECK(poll(&no_request, 1, 100) == 0, "pasted newlines never submit");
    keys(&pty, "\x15生成代码\r");
    Json *question = client_frame(&pty);
    CHECK(!strcmp(json_string(JV(question), "text", ""), "生成代码"), "Chinese prompt intact");
    json_free(question);
    wait_text(&pty, "思考中");
    simple_event(&pty, "{\"type\":\"text_delta\",\"text\":\"CHAT_STREAM_ONE\"}", "1");
    simple_event(&pty,
                 "{\"type\":\"tool_start\",\"name\":\"Write\",\"args\":{\"file_path\":\"src/"
                 "main.c\",\"content\":\"CODE_MARKER_4862\"}}",
                 "1");
    simple_event(
        &pty, "{\"type\":\"tool_result\",\"name\":\"Write\",\"result\":{\"ok\":true,\"bytes\":16}}",
        "1");
    simple_event(&pty,
                 "{\"type\":\"tool_start\",\"name\":\"Bash\",\"args\":{\"command\":\"make test\"}}",
                 "1");
    simple_event(&pty, "{\"type\":\"tool_output\",\"text\":\"BUILD_MARKER_9217\"}", "1");
    wait_text(&pty, "Bash");
    CHECK(!strstr(buf_text(&pty.screen), "CODE_MARKER_4862") &&
              !strstr(buf_text(&pty.screen), "BUILD_MARKER_9217"),
          "tool details collapsed during generation");
    keys(&pty, "\t\r");
    wait_text(&pty, "BUILD_MARKER_9217");
    keys(&pty, "\x1b[<0;5;8M");
    wait_text(&pty, "CODE_MARKER_4862");
    keys(&pty, "\x1b");
    sleep_ms(40);
    keys(&pty, "下一轮草稿\r");
    wait_text(&pty, "结束后再发送");
    CHECK(poll(&no_request, 1, 100) == 0, "busy editor keeps draft without queueing task");
    simple_event(&pty, "{\"type\":\"tool_output\",\"text\":\"BUILD_STREAM_UPDATE\"}", "1");
    wait_text(&pty, "BUILD_STREAM_UPDATE");
    size = (struct winsize){.ws_row = 12, .ws_col = 78};
    CHECK(ioctl(pty.terminal, TIOCSWINSZ, &size) == 0, "resize PTY");
    kill(pty.pid, SIGWINCH);
    buf_clear(&pty.screen);
    wait_text(&pty, "Token");
    wait_text(&pty, "qwen3.5-4b");
    simple_event(
        &pty,
        "{\"type\":\"tool_result\",\"name\":\"Bash\",\"result\":{\"ok\":true,\"exit_code\":0}}",
        "1");
    simple_event(&pty, "{\"type\":\"task_done\",\"status\":\"success\"}", "1");
    sleep_ms(100);
    keys(&pty, "\r");
    question = client_frame(&pty);
    CHECK(!strcmp(json_string(JV(question), "text", ""), "下一轮草稿"),
          "draft submits after completion");
    json_free(question);
    simple_event(
        &pty, "{\"type\":\"tool_start\",\"name\":\"Read\",\"args\":{\"file_path\":\"src/main.c\"}}",
        "2");
    simple_event(&pty, "{\"type\":\"task_done\",\"status\":\"cancelled\"}", "2");
    wait_text(&pty, "已中止");
    size = (struct winsize){.ws_row = 26, .ws_col = 90};
    CHECK(ioctl(pty.terminal, TIOCSWINSZ, &size) == 0, "restore PTY size");
    kill(pty.pid, SIGWINCH);
    sleep_ms(80);
    Buffer prompt = {0};
    OK(buf_append(&prompt, "PROMPT_BEGIN\n", 13, &error));
    for (int i = 0; i < 60; ++i)
        OK(buf_printf(&prompt, &error, "multiline row %d\n", i));
    OK(buf_append(&prompt, "EDITOR_BOTTOM", 13, &error));
    keys(&pty, "\x1b[200~");
    terminal_write(&pty, prompt.data, prompt.len);
    keys(&pty, "\x1b[201~");
    wait_text(&pty, "EDITOR_BOTTOM");
    keys(&pty, "\r");
    question = client_frame(&pty);
    JVal *submitted = json_get(JV(question), "text");
    CHECK(json_len(submitted) == prompt.len &&
              !memcmp(json_str(submitted), prompt.data, prompt.len),
          "oversized multiline prompt preserved as one request");
    json_free(question);
    buf_free(&prompt);
    simple_event(
        &pty, "{\"type\":\"tool_start\",\"name\":\"Glob\",\"args\":{\"pattern\":\"**/*\"}}", "3");
    Json *result = json_event("tool_result");
    json_set_string(result, "name", "Glob");
    Json *payload = json_object(), *files = json_array();
    json_set_bool(payload, "ok", true);
    for (int i = 0; i < 150; ++i) {
        char name[64];
        snprintf(name, sizeof(name),
                 i == 0     ? "JSON_HEAD_MARKER"
                 : i == 149 ? "JSON_TAIL_MARKER"
                            : "result_%03d.c",
                 i);
        OK(json_append_string(files, name));
    }
    json_set_value(payload, "files", JV(files));
    json_set_value(result, "result", JV(payload));
    json_free(payload);
    json_free(files);
    server_event(&pty, result, "3");
    wait_text(&pty, "Glob");
    keys(&pty, "\t\r");
    sleep_ms(80);
    drain_terminal(&pty);
    CHECK(!strstr(buf_text(&pty.screen), "JSON_TAIL_MARKER"),
          "long JSON tail starts below viewport");
    wheel(&pty, true, 60);
    wait_text(&pty, "JSON_TAIL_MARKER");
    buf_clear(&pty.screen);
    wheel(&pty, false, 60);
    wait_text(&pty, "JSON_HEAD_MARKER");
    keys(&pty, "\x1b[M\x61\x25\x25");
    sleep_ms(50);
    drain_terminal(&pty);
    keys(&pty, "\x1b[<65;");
    sleep_ms(80);
    buf_clear(&pty.screen);
    simple_event(&pty, "{\"type\":\"context_usage\",\"context_tokens\":7777}", "3");
    wait_text(&pty, "7777");
    keys(&pty, "5;5M");
    simple_event(&pty, "{\"type\":\"task_done\",\"status\":\"success\"}", "3");
    sleep_ms(80);
    stop_terminal(&pty, 1);
    free(directory);
    puts("PASS real C PTY: Unicode, paste, cards, draft, resize, full JSON, mouse fragments and "
         "restoration");
}
int main(int argc, char **argv) {
    test_init();
    CHECK(argc == 2, "client binary required");
    ui_state_tests();
    ui_history_tests();
    resume_terminal_tests(argv[1]);
    terminal_tests(argv[1]);
    test_success = true;
    puts("All C UI tests passed.");
    return 0;
}
