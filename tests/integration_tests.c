#include "test_support.h"
#include <fcntl.h>
#include <strings.h>

typedef struct {
    char path[128];
    Json *body;
} HttpRequest;
typedef struct {
    int socket, sequence;
    Json *hello, *events;
    Buffer answer;
} Connection;
static HttpRequest read_http(int fd) {
    Buffer bytes = {0};
    size_t header_size = 0, content_size = 0;
    for (;;) {
        char *end = strstr(buf_text(&bytes), "\r\n\r\n");
        if (end && !header_size) {
            header_size = (size_t)(end - bytes.data) + 4;
            char *length = strstr(bytes.data, "Content-Length:");
            if (length)
                content_size = (size_t)strtoul(length + 15, NULL, 10);
            CHECK(content_size <= 8 * CA_FRAME_LIMIT, "HTTP request bounded");
        }
        if (header_size && bytes.len >= header_size + content_size)
            break;
        struct pollfd wait = {fd, POLLIN, 0};
        CHECK(poll(&wait, 1, 5000) > 0, "HTTP request arrives");
        char chunk[4096];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        CHECK(n > 0, "complete HTTP request");
        OK(buf_append(&bytes, chunk, (size_t)n, &error));
        CHECK(bytes.len <= 8 * CA_FRAME_LIMIT + 8192, "HTTP request limit");
    }
    HttpRequest request = {0};
    char method[16];
    CHECK(sscanf(bytes.data, "%15s %127s", method, request.path) == 2, "HTTP request line");
    request.body =
        content_size ? json_parse(bytes.data + header_size, content_size) : json_object();
    CHECK(request.body, "HTTP JSON body");
    buf_free(&bytes);
    return request;
}
static void json_response(int fd, const char *literal) {
    Buffer header = {0};
    OK(buf_printf(&header, &error,
                  "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                  "%zu\r\nConnection: close\r\n\r\n",
                  strlen(literal)));
    raw_send(fd, header.data, header.len, 0);
    raw_send(fd, literal, strlen(literal), 0);
    buf_free(&header);
}
static void send_sse(int fd, Json *event) {
    size_t length;
    char *text = json_dump(event, false, &length);
    CHECK(text, "serialize SSE");
    raw_send(fd, "data: ", 6, 0);
    raw_send(fd, text, length, 7);
    raw_send(fd, "\r\n\r\n", 4, 0);
    free(text);
    json_free(event);
}
static void delta(int fd, const char *text, const char *finish) {
    Json *event = json_literal("{\"choices\":[{\"delta\":{}}]}");
    JVal *choice = json_at(json_get(JV(event), "choices"), 0);
    if (text)
        json_add_string(event, json_get(choice, "delta"), "content", text, strlen(text));
    if (finish)
        json_add_string(event, choice, "finish_reason", finish, strlen(finish));
    send_sse(fd, event);
}
static void text_reply(int fd, const char *text, int repeats, bool usage, bool done) {
    for (int i = 0; i < repeats; ++i) {
        delta(fd, text, NULL);
        sleep_ms(15);
    }
    delta(fd, NULL, "stop");
    if (usage)
        send_sse(fd,
                 json_literal(
                     "{\"choices\":[],\"usage\":{\"prompt_tokens\":512,\"completion_tokens\":7}}"));
    if (done)
        raw_send(fd, "data: [DONE]\n\n", 14, 0);
}
static void tool_patch(int fd, const char *name, size_t name_len, const char *args, size_t args_len,
                       bool first, int index) {
    Json *event = json_literal(
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{}}]}}]}");
    JVal *patch = json_at(
        json_get(json_get(json_at(json_get(JV(event), "choices"), 0), "delta"), "tool_calls"), 0);
    JVal *function = json_get(patch, "function");
    json_add(event, patch, "index", yyjson_mut_sint(event->doc, index));
    if (first)
        json_add_string(event, patch, "id", "call_1", 6);
    json_add_string(event, function, "name", name, name_len);
    json_add_string(event, function, "arguments", args, args_len);
    send_sse(fd, event);
}
static void tool_reply(int fd, const char *name, Json *args, bool truncated, bool invalid_args,
                       int index) {
    size_t n;
    char *text = invalid_args ? ca_strdup("{invalid", &error) : json_dump(args, false, &n);
    CHECK(text, "tool arguments");
    if (invalid_args)
        n = strlen(text);
    delta(fd, "正在处理", NULL);
    size_t middle = utf8_prefix_length(text, n / 2, false), name_middle = strlen(name) / 2;
    tool_patch(fd, name, name_middle, text, middle, true, index);
    tool_patch(fd, name + name_middle, strlen(name) - name_middle, text + middle, n - middle, false,
               index);
    delta(fd, NULL, truncated ? "length" : "tool_calls");
    raw_send(fd, "data: [DONE]\n\n", 14, 0);
    free(text);
    json_free(args);
}
static Json *write_source(void) {
    Buffer source = fixture_source();
    Json *args = json_object();
    json_set_string(args, "file_path", "src/main.c");
    json_set_stringn(args, "content", source.data, source.len);
    buf_free(&source);
    return args;
}
static void mock_connection(int fd) {
    HttpRequest request = read_http(fd);
    if (!strcmp(request.path, "/props")) {
        json_response(fd, "{\"total_slots\":2}");
        goto done;
    }
    if (!strcmp(request.path, "/slots")) {
        json_response(fd, "[{\"n_ctx\":32768},{\"n_ctx\":32768}]");
        goto done;
    }
    JVal *messages = json_get(JV(request.body), "messages");
    CHECK(json_is_array(messages) && json_is_array(json_get(JV(request.body), "tools")),
          "full history and tools sent");
    size_t last = 0;
    int tools = 0;
    for (size_t i = 0; i < json_len(messages); ++i)
        if (!strcmp(json_string(json_at(messages, i), "role", ""), "user"))
            last = i;
    const char *prompt = json_string(json_at(messages, last), "content", "");
    for (size_t i = last + 1; i < json_len(messages); ++i)
        if (!strcmp(json_string(json_at(messages, i), "role", ""), "tool"))
            ++tools;
    if (!strcmp(request.path, "/v1/chat/completions/input_tokens")) {
        if (!strcmp(prompt, "BAD_COUNT"))
            json_response(fd, "{\"input_tokens\":1.5}");
        else
            json_response(fd, !strcmp(prompt, "COUNT_OVER") ||
                                      (!strcmp(prompt, "GENERATE_OVER_COUNT") && tools >= 3)
                                  ? "{\"input_tokens\":30000}"
                                  : "{\"input_tokens\":512}");
        goto done;
    }
    CHECK(!strcmp(json_string(JV(request.body), "tool_choice", ""), "auto"),
          "successful command does not disable tools");
    if (!strcmp(prompt, "HTTP_ERROR")) {
        const char *failure = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                              "7\r\nConnection: close\r\n\r\nfailure";
        raw_send(fd, failure, strlen(failure), 0);
        goto done;
    }
    const char *header =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
    raw_send(fd, header, strlen(header), 0);
    if (!strcmp(prompt, "GENERATE") || !strcmp(prompt, "EXTEND") ||
        !strcmp(prompt, "KEEP_EDITING") || !strcmp(prompt, "GENERATE_OVER_COUNT")) {
        if (!tools)
            tool_reply(fd, "Read",
                       json_literal(!strcmp(prompt, "EXTEND") ? "{\"file_path\":\"src/main.c\"}"
                                                              : "{\"file_path\":\"README.md\"}"),
                       false, false, 0);
        else if (tools == 1 && !strcmp(prompt, "EXTEND")) {
            Json *args = json_object();
            json_set_string(args, "file_path", "src/main.c");
            json_set_string(args, "old_string", "    sort(values, count);\n");
            json_set_string(args, "new_string",
                            "    sort(values, count);\n    if (argc == 2 && !strcmp(argv[1], "
                            "\"--descending\")) reverse(values, count);\n");
            tool_reply(fd, "Edit", args, false, false, 0);
        } else if (tools == 1)
            tool_reply(fd, "Write", write_source(), false, false, 0);
        else if (tools == 2 || (!strcmp(prompt, "KEEP_EDITING") && tools == 4))
            tool_reply(fd, "Bash",
                       json_literal("{\"command\":\"make --no-print-directory "
                                    "test\",\"description\":\"编译并运行自测\"}"),
                       false, false, 0);
        else if (!strcmp(prompt, "KEEP_EDITING") && tools == 3)
            tool_reply(
                fd, "Edit",
                json_literal("{\"file_path\":\"src/main.c\",\"old_string\":\"return (a > b) - (a < "
                             "b);\",\"new_string\":\"return a < b ? -1 : a > b ? 1 : 0;\"}"),
                false, false, 0);
        else
            text_reply(fd, "完成", 3, false, true);
    } else if (!strcmp(prompt, "GENERIC")) {
        if (!tools)
            tool_reply(fd, "Glob", json_literal("{\"pattern\":\"**/*\"}"), false, false, 0);
        else if (tools == 1)
            tool_reply(
                fd, "Write",
                json_literal(
                    "{\"file_path\":\"main.sh\",\"content\":\"printf 'generic shell\\\\n'\\n\"}"),
                false, false, 0);
        else if (tools == 2 || tools == 4)
            tool_reply(fd, "Bash", json_literal("{\"command\":\"bash main.sh\"}"), false, false, 0);
        else if (tools == 3)
            tool_reply(fd, "Edit",
                       json_literal("{\"file_path\":\"main.sh\",\"old_string\":\"generic "
                                    "shell\",\"new_string\":\"updated shell\"}"),
                       false, false, 0);
        else
            text_reply(fd, "done", 1, false, true);
    } else if (!strcmp(prompt, "UNVERIFIED")) {
        if (!tools)
            tool_reply(fd, "Write", write_source(), false, false, 0);
        else
            text_reply(fd, "未运行测试", 1, false, true);
    } else if (!strcmp(prompt, "FAILED_COMMAND")) {
        if (!tools)
            tool_reply(fd, "Bash",
                       json_literal("{\"command\":\"printf 'command failed\\\\n'; exit 5\"}"),
                       false, false, 0);
        else
            text_reply(fd, "exit 5", 1, false, true);
    } else if (!strcmp(prompt, "INVALID_ARGS")) {
        if (!tools)
            tool_reply(fd, "Write", write_source(), false, true, 0);
        else
            text_reply(fd, "invalid arguments reported", 1, false, true);
    } else if (!strcmp(prompt, "TRUNCATED"))
        tool_reply(fd, "Write", write_source(), true, false, 0);
    else if (!strcmp(prompt, "BAD_INDEX"))
        tool_reply(fd, "Write", write_source(), false, false, 16);
    else if (!strcmp(prompt, "LOOP"))
        tool_reply(fd, "Glob", json_literal("{\"pattern\":\"**/*\"}"), false, false, 0);
    else if (!strcmp(prompt, "RECALL")) {
        const char *marker = "missing";
        for (size_t i = 0; i < last; ++i)
            if (!strcmp(json_string(json_at(messages, i), "role", ""), "user")) {
                const char *earlier = json_string(json_at(messages, i), "content", "");
                if (!strncmp(earlier, "REMEMBER ", 9))
                    marker = earlier + 9;
            }
        text_reply(fd, marker, 4, false, true);
    } else if (!strcmp(prompt, "USAGE"))
        text_reply(fd, "token statistics", 1, true, true);
    else if (!strcmp(prompt, "MISSING_DONE"))
        text_reply(fd, "partial answer", 1, false, false);
    else
        text_reply(fd, prompt, !strcmp(prompt, "DISCONNECT") || !strcmp(prompt, "SURVIVE") ? 60 : 8,
                   false, true);
done:
    json_free(request.body);
}
static int start_mock(void) {
    int listener = listen_tcp("127.0.0.1", 0, &error);
    CHECK(listener >= 0, "mock listener");
    int port = socket_port(listener, &error);
    pid_t child = fork();
    CHECK(child >= 0, "fork mock");
    if (!child) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        for (;;) {
            while (waitpid(-1, NULL, WNOHANG) > 0) {
            }
            struct pollfd wait = {listener, POLLIN, 0};
            if (poll(&wait, 1, 100) <= 0)
                continue;
            int fd = accept(listener, NULL, NULL);
            if (fd < 0)
                continue;
            pid_t request = fork();
            CHECK(request >= 0, "fork mock request");
            if (!request) {
                close(listener);
                mock_connection(fd);
                close(fd);
                _exit(0);
            }
            close(fd);
        }
    }
    register_process(child);
    close(listener);
    return port;
}
static int reserve_port(void) {
    int fd = listen_tcp("127.0.0.1", 0, &error);
    CHECK(fd >= 0, "reserve port");
    int port = socket_port(fd, &error);
    close(fd);
    return port;
}
static int start_server(const char *binary, int model_port, const char *sessions, int rounds) {
    int port = reserve_port();
    char port_text[16], url[64], round_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", model_port);
    snprintf(round_text, sizeof(round_text), "%d", rounds);
    pid_t child = fork();
    CHECK(child >= 0, "fork server");
    if (!child) {
        setpgid(0, 0);
        char *log = test_path("server.log");
        int fd = open(log, O_WRONLY | O_APPEND | O_CREAT, 0600);
        free(log);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execl(binary, binary, "--port", port_text, "--model-url", url, "--sessions", sessions,
              "--max-rounds", round_text, (char *)NULL);
        _exit(127);
    }
    register_process(child);
    for (int i = 0; i < 100; ++i) {
        CaError local = {0};
        int fd = connect_tcp("127.0.0.1", port, &local);
        if (fd >= 0) {
            close(fd);
            return port;
        }
        sleep_ms(30);
    }
    CHECK(false, "server starts");
    return -1;
}
static Connection connection(int port) {
    Connection c = {.socket = -1};
    c.socket = connect_tcp("127.0.0.1", port, &error);
    CHECK(c.socket >= 0, "client connects");
    CHECK(receive_frame(c.socket, &c.hello, 5000, &error) == 1 &&
              !strcmp(json_string(JV(c.hello), "type", ""), "hello"),
          "hello received");
    return c;
}
static void connection_close(Connection *c) {
    close_fd(&c->socket);
    json_free(c->hello);
    json_free(c->events);
    buf_free(&c->answer);
    memset(c, 0, sizeof(*c));
    c->socket = -1;
}
static void wait_gone(pid_t pid) {
    for (int i = 0; i < 150; ++i) {
        if (kill(pid, 0) < 0 && errno == ESRCH)
            return;
        sleep_ms(20);
    }
    CHECK(false, "session child reaped");
}
static Json *control(Connection *c, const char *type, const char *key, const char *value) {
    Json *request = json_event(type);
    if (key)
        json_set_string(request, key, value);
    OK(send_frame(c->socket, JV(request), 5000, &error));
    json_free(request);
    Json *reply = NULL;
    CHECK(receive_frame(c->socket, &reply, 5000, &error) == 1, "control response");
    return reply;
}
static void ask(Connection *c, const char *text) {
    json_free(c->events);
    c->events = json_array();
    buf_clear(&c->answer);
    char id[24];
    snprintf(id, sizeof(id), "%d", ++c->sequence);
    Json *request = json_event("user_message");
    json_set_string(request, "request_id", id);
    json_set_string(request, "text", text);
    OK(send_frame(c->socket, JV(request), 5000, &error));
    json_free(request);
}
static bool receive_event(Connection *c) {
    Json *event = NULL;
    char id[24];
    snprintf(id, sizeof(id), "%d", c->sequence);
    CHECK(receive_frame(c->socket, &event, 15000, &error) == 1, "event before EOF");
    CHECK(!strcmp(json_string(JV(event), "request_id", ""), id), "matching request ID");
    OK(json_append(c->events, JV(event)));
    if (!strcmp(json_string(JV(event), "type", ""), "text_delta")) {
        JVal *text = json_get(JV(event), "text");
        OK(buf_append(&c->answer, json_str(text), json_len(text), &error));
    }
    bool done = !strcmp(json_string(JV(event), "type", ""), "task_done");
    json_free(event);
    return done;
}
static const char *collect(Connection *c) {
    while (!receive_event(c)) {
    }
    return json_string(json_at(JV(c->events), json_len(JV(c->events)) - 1), "status", "");
}
static void task(Connection *c, const char *prompt, const char *status) {
    ask(c, prompt);
    CHECK(!strcmp(collect(c), status), prompt);
}
static JVal *last_tool(Connection *c, const char *name) {
    JVal *result = NULL;
    for (size_t i = 0; i < json_len(JV(c->events)); ++i) {
        JVal *event = json_at(JV(c->events), i);
        if (!strcmp(json_string(event, "type", ""), "tool_result") &&
            !strcmp(json_string(event, "name", ""), name))
            result = json_get(event, "result");
    }
    return result;
}
static void generation_tests(int port) {
    Connection a = connection(port), b = connection(port);
    CHECK(strcmp(json_string(JV(a.hello), "session_id", ""),
                 json_string(JV(b.hello), "session_id", "")) &&
              json_int(JV(a.hello), "pid", 0) != json_int(JV(b.hello), "pid", 0),
          "sessions and processes isolated");
    task(&a, "GENERATE", "success");
    CHECK(json_bool(last_tool(&a, "Bash"), "ok", false),
          "model-generated C actually compiles and self-tests");
    char *file = path_join(json_string(JV(a.hello), "workspace", ""), "src/main.c", &error);
    char *other = path_join(json_string(JV(b.hello), "workspace", ""), "src/main.c", &error);
    Buffer source = read_text(file), fixture = fixture_source();
    CHECK(source.len == fixture.len && !memcmp(source.data, fixture.data, source.len) &&
              access(other, F_OK) < 0,
          "real generated files isolated");
    buf_free(&source);
    buf_free(&fixture);
    task(&a, "EXTEND", "success");
    CHECK(json_bool(last_tool(&a, "Bash"), "ok", false),
          "same connection extends and recompiles C");
    source = read_text(file);
    CHECK(strstr(source.data, "--descending"), "requested extension saved");
    buf_free(&source);
    Json *late = control(&a, "set_workspace", "workspace", test_root);
    CHECK(!strcmp(json_string(JV(late), "type", ""), "workspace_error"),
          "directory change rejected after task before resume");
    json_free(late);
    task(&b, "KEEP_EDITING", "success");
    CHECK(json_bool(last_tool(&b, "Bash"), "ok", false),
          "tools can continue after successful build");
    CHECK(json_int(json_at(JV(b.events), json_len(JV(b.events)) - 1), "rounds", 0) == 6,
          "all model/tool rounds performed");
    free(file);
    free(other);
    connection_close(&a);
    connection_close(&b);
    Connection generic = connection(port);
    char *directory = test_path("generic work");
    Json *ready = control(&generic, "set_workspace", "workspace", directory);
    CHECK(!strcmp(json_string(JV(ready), "type", ""), "workspace_ready"),
          "empty generic workdir accepted");
    json_free(ready);
    task(&generic, "GENERIC", "success");
    CHECK(json_bool(last_tool(&generic, "Bash"), "ok", false) &&
              strstr(json_string(last_tool(&generic, "Bash"), "output", ""), "updated shell"),
          "generic directory uses all tools without fixed build contract");
    char *makefile = path_join(directory, "Makefile", &error);
    CHECK(access(makefile, F_OK) < 0, "direct workdir is not replaced by template");
    free(makefile);
    free(directory);
    connection_close(&generic);
    puts("PASS generated C, real compile/self-test, continuous edits and generic workdirs");
}
static void session_tests(int port) {
    Connection origin = connection(port);
    char *id = ca_strdup(json_string(JV(origin.hello), "session_id", ""), &error),
         *logs = ca_strdup(json_string(JV(origin.hello), "logs", ""), &error);
    task(&origin, "REMEMBER 会话标记-A", "success");
    task(&origin, "GENERATE", "success");
    char *first_log = path_join(logs, "command-1.log", &error);
    Buffer before = read_text(first_log);
    Connection active = connection(port);
    Json *reply = control(&active, "resume_session", "session_id", id);
    CHECK(!strcmp(json_string(JV(reply), "type", ""), "session_error"),
          "active session cannot be resumed concurrently");
    json_free(reply);
    connection_close(&active);
    pid_t pid = (pid_t)json_int(JV(origin.hello), "pid", 0);
    connection_close(&origin);
    wait_gone(pid);
    Connection back = connection(port);
    reply = control(&back, "list_sessions", NULL, NULL);
    bool found = false;
    JVal *sessions = json_get(JV(reply), "sessions");
    for (size_t i = 0; i < json_len(sessions); ++i)
        if (!strcmp(json_string(json_at(sessions, i), "session_id", ""), id))
            found = true;
    CHECK(found, "saved session listed");
    json_free(reply);
    task(&back, "RECALL", "success");
    CHECK(!strstr(buf_text(&back.answer), "会话标记-A"), "new session has no old context");
    reply = control(&back, "resume_session", "session_id", id);
    CHECK(!strcmp(json_string(JV(reply), "type", ""), "session_ready"),
          "resume on existing connection");
    json_free(reply);
    task(&back, "RECALL", "success");
    CHECK(strstr(buf_text(&back.answer), "会话标记-A"), "restored history sent to model");
    task(&back, "EXTEND", "success");
    CHECK(json_bool(last_tool(&back, "Bash"), "ok", false), "restored workspace compiles");
    Buffer after = read_text(first_log);
    CHECK(before.len == after.len && !memcmp(before.data, after.data, before.len),
          "resume never overwrites earlier command log");
    buf_free(&before);
    buf_free(&after);
    char *second_log = path_join(logs, "command-2.log", &error);
    CHECK(access(second_log, F_OK) == 0, "resumed command log increments");
    free(second_log);
    char *elsewhere = test_path("resumed elsewhere");
    reply = control(&back, "set_workspace", "workspace", elsewhere);
    CHECK(!strcmp(json_string(JV(reply), "type", ""), "workspace_ready"),
          "resumed session can change workdir");
    json_free(reply);
    task(&back, "FAILED_COMMAND", "success");
    CHECK(json_int(last_tool(&back, "Bash"), "exit_code", 0) == 5,
          "command status remains separate from task status");
    char *third_log = path_join(logs, "command-3.log", &error);
    CHECK(access(third_log, F_OK) == 0, "directory switch keeps log sequence");
    free(third_log);
    reply = control(&back, "resume_session", "session_id", id);
    CHECK(!strcmp(json_string(JV(reply), "type", ""), "session_error"),
          "active current session rejected");
    json_free(reply);
    pid = (pid_t)json_int(JV(back.hello), "pid", 0);
    connection_close(&back);
    wait_gone(pid);
    char *events_file = path_join(logs, "events.jsonl", &error);
    Buffer events = read_text(events_file);
    char *last = events.data, *cursor = events.data;
    while ((cursor = strchr(cursor, '\n'))) {
        if (cursor[1])
            last = cursor + 1;
        ++cursor;
    }
    Json *closed = json_literal(last);
    CHECK(closed && !strcmp(json_string(JV(closed), "type", ""), "session_closed") &&
              !strcmp(json_string(JV(closed), "session_id", ""), id),
          "close event uses resumed session identity");
    json_free(closed);
    buf_free(&events);
    free(events_file);
    char *blocked = test_path("blocked-workspace"), *marker = path_join(logs, ".workspace", &error);
    OK(write_file_bytes(blocked, "file", 4, &error));
    Buffer mark = {0};
    OK(buf_printf(&mark, &error, "%s\n", blocked));
    OK(write_file_bytes(marker, mark.data, mark.len, &error));
    buf_free(&mark);
    Connection fallback = connection(port);
    char *fallback_logs = ca_strdup(json_string(JV(fallback.hello), "logs", ""), &error);
    reply = control(&fallback, "resume_session", "session_id", id);
    CHECK(!strcmp(json_string(JV(reply), "type", ""), "session_ready") &&
              json_get(JV(reply), "warning"),
          "unavailable original workdir yields explicit fallback");
    json_free(reply);
    task(&fallback, "FAILED_COMMAND", "success");
    char *fourth_log = path_join(logs, "command-4.log", &error),
         *wrong_log = path_join(fallback_logs, "command-1.log", &error);
    CHECK(access(fourth_log, F_OK) == 0 && access(wrong_log, F_OK) < 0,
          "fallback commands use restored session logs");
    connection_close(&fallback);
    free(id);
    free(logs);
    free(first_log);
    free(elsewhere);
    free(blocked);
    free(marker);
    free(fallback_logs);
    free(fourth_log);
    free(wrong_log);
    puts("PASS session recovery, locks, log ownership and non-overwrite, fallback");
}
static void concurrent_tests(int port) {
    Connection a = connection(port), b = connection(port);
    ask(&a, "REMEMBER alpha");
    ask(&b, "REMEMBER beta");
    bool done[2] = {false, false};
    int chunks[2] = {0, 0};
    Connection *clients[] = {&a, &b};
    while (!done[0] || !done[1]) {
        struct pollfd fds[] = {{done[0] ? -1 : a.socket, POLLIN, 0},
                               {done[1] ? -1 : b.socket, POLLIN, 0}};
        CHECK(poll(fds, 2, 5000) > 0, "parallel streams arrive");
        for (int i = 0; i < 2; ++i)
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                done[i] = receive_event(clients[i]);
                JVal *event = json_at(JV(clients[i]->events), json_len(JV(clients[i]->events)) - 1);
                if (!strcmp(json_string(event, "type", ""), "text_delta"))
                    ++chunks[i];
                if (done[i])
                    CHECK(chunks[0] > 1 && chunks[1] > 1,
                          "both streams overlap before either completes");
            }
    }
    task(&a, "RECALL", "success");
    task(&b, "RECALL", "success");
    CHECK(strstr(buf_text(&a.answer), "alpha") && !strstr(buf_text(&a.answer), "beta") &&
              strstr(buf_text(&b.answer), "beta") && !strstr(buf_text(&b.answer), "alpha"),
          "history isolation");
    ask(&a, "DISCONNECT");
    ask(&b, "SURVIVE");
    while (!strstr(buf_text(&a.answer), "DISCONNECT"))
        CHECK(!receive_event(&a), "A starts before close");
    pid_t pid = (pid_t)json_int(JV(a.hello), "pid", 0);
    connection_close(&a);
    CHECK(!strcmp(collect(&b), "success") && strstr(buf_text(&b.answer), "SURVIVE"),
          "B survives A disconnect");
    wait_gone(pid);
    connection_close(&b);
    puts("PASS overlapping streams, context isolation and disconnect cleanup");
}
static void failure_tests(int port) {
    const char *prompts[] = {"TRUNCATED", "BAD_INDEX",  "MISSING_DONE", "HTTP_ERROR",
                             "BAD_COUNT", "COUNT_OVER", "LOOP",         "GENERATE_OVER_COUNT"};
    const char *statuses[] = {"failed", "failed",        "failed", "failed",
                              "failed", "context_limit", "limit",  "context_limit"};
    for (size_t i = 0; i < sizeof(prompts) / sizeof(*prompts); ++i) {
        Connection c = connection(port);
        task(&c, prompts[i], statuses[i]);
        if (i < 2) {
            char *file = path_join(json_string(JV(c.hello), "workspace", ""), "src/main.c", &error);
            CHECK(access(file, F_OK) < 0, "incomplete tool output never executes");
            free(file);
        }
        if (!strcmp(prompts[i], "GENERATE_OVER_COUNT"))
            CHECK(json_bool(last_tool(&c, "Bash"), "ok", false),
                  "context limit still reported after a real successful build");
        connection_close(&c);
    }
    Connection c = connection(port);
    task(&c, "INVALID_ARGS", "success");
    CHECK(!json_bool(last_tool(&c, "Write"), "ok", true),
          "invalid arguments returned as tool error");
    task(&c, "UNVERIFIED", "success");
    CHECK(!last_tool(&c, "Bash") && strstr(buf_text(&c.answer), "未运行测试"),
          "task completion never fabricates validation");
    task(&c, "USAGE", "success");
    int measured = -1;
    for (size_t i = 0; i < json_len(JV(c.events)); ++i) {
        JVal *event = json_at(JV(c.events), i);
        if (!strcmp(json_string(event, "type", ""), "context_usage"))
            measured = (int)json_int(event, "context_tokens", -1);
    }
    CHECK(measured == 519, "measured input and output token usage");
    connection_close(&c);
    puts("PASS invalid/truncated tools, HTTP failures, token and round limits, true usage");
}
static Buffer run_cli(const char *binary, int port, const char *mode, const char *prompt,
                      const char *resume, int *exit_code) {
    int ends[2];
    CHECK(pipe(ends) == 0, "client output pipe");
    char number[16];
    snprintf(number, sizeof(number), "%d", port);
    pid_t child = fork();
    CHECK(child >= 0, "fork command-line client");
    if (!child) {
        setpgid(0, 0);
        close(ends[0]);
        dup2(ends[1], STDOUT_FILENO);
        close(ends[1]);
        const char *args[12] = {binary, "--port", number, mode, "--prompt", prompt};
        size_t count = 6;
        if (resume) {
            args[count++] = "--resume";
            args[count++] = resume;
        }
        args[count] = NULL;
        execv(binary, (char *const *)args);
        _exit(127);
    }
    register_process(child);
    close(ends[1]);
    Buffer output = {0};
    char bytes[4096];
    ssize_t n;
    while ((n = read(ends[0], bytes, sizeof(bytes))) > 0)
        OK(buf_append(&output, bytes, (size_t)n, &error));
    close(ends[0]);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status), "client exits normally");
    *exit_code = WEXITSTATUS(status);
    return output;
}
static void append_long_reply(const char *logs) {
    Buffer content = {0};
    char block[1024];
    memset(block, 'x', sizeof(block));
    memcpy(block + sizeof(block) - 3, "中", 3);
    OK(buf_append(&content, "SNAPSHOT_LONG_HEAD\n", 19, &error));
    while (content.len <= CA_FRAME_LIMIT)
        OK(buf_append(&content, block, sizeof(block), &error));
    OK(buf_append(&content, "\nSNAPSHOT_LONG_TAIL", 19, &error));
    Json *item = json_object(), *message = json_object();
    json_set_string(message, "role", "assistant");
    json_set_stringn(message, "content", content.data, content.len);
    json_set_value(item, "message", JV(message));
    char *path = path_join(logs, "history.jsonl", &error);
    FILE *file = fopen(path, "a");
    CHECK(file, "open saved history");
    OK(append_json(file, JV(item), &error));
    CHECK(fclose(file) == 0, "close saved history");
    free(path);
    json_free(item);
    json_free(message);
    buf_free(&content);
}
static void check_history_chunks(const Buffer *output) {
    Buffer history = {0};
    int chunks = 0;
    bool done = false;
    for (const char *line = output->data; line && *line;) {
        const char *end = strchr(line, '\n');
        CHECK(end, "JSON CLI line complete");
        Json *event = json_parse(line, (size_t)(end - line));
        CHECK(event, "JSON CLI line valid");
        if (!strcmp(json_string(JV(event), "type", ""), "session_history")) {
            JVal *text = json_get(JV(event), "text");
            CHECK(!done && json_is_string(text) && json_len(text) <= 64 * 1024,
                  "history frames bounded and ordered");
            OK(buf_append(&history, json_str(text), json_len(text), &error));
            done = json_bool(JV(event), "done", false);
            ++chunks;
        }
        json_free(event);
        line = end + 1;
    }
    Json *snapshot = json_parse(buf_text(&history), history.len);
    CHECK(done && chunks > 16 && snapshot && json_is_array(JV(snapshot)),
          "history exceeding one TCP frame restored through multiple chunks");
    JVal *reply = json_at(JV(snapshot), json_len(JV(snapshot)) - 1);
    CHECK(json_len(json_get(reply, "content")) > CA_FRAME_LIMIT &&
              strstr(json_string(reply, "content", ""), "SNAPSHOT_LONG_HEAD") &&
              strstr(json_string(reply, "content", ""), "SNAPSHOT_LONG_TAIL"),
          "long Unicode history received without truncation");
    json_free(snapshot);
    buf_free(&history);
}
static void cli_tests(const char *binary, int port) {
    int code;
    Buffer output = run_cli(binary, port, "--json", "USAGE", NULL, &code);
    CHECK(code == 0 && strstr(buf_text(&output), "context_tokens\":519") &&
              strstr(buf_text(&output), "task_done"),
          "JSON CLI events");
    buf_free(&output);
    output = run_cli(binary, port, "--plain", "hello", NULL, &code);
    CHECK(code == 0 && strstr(buf_text(&output), "hello") &&
              strstr(buf_text(&output), "[本轮 success]"),
          "plain CLI streams");
    buf_free(&output);
    output = run_cli(binary, port, "--verbose", "FAILED_COMMAND", NULL, &code);
    CHECK(code == 0 && strstr(buf_text(&output), "command failed") &&
              strstr(buf_text(&output), "exit_code"),
          "verbose includes complete tool result");
    buf_free(&output);
    output = run_cli(binary, port, "--json", "COUNT_OVER", NULL, &code);
    CHECK(code == 1 && strstr(buf_text(&output), "context_limit"), "CLI failure exit code");
    buf_free(&output);
    Connection c = connection(port);
    task(&c, "REMEMBER cli_marker", "success");
    char *id = ca_strdup(json_string(JV(c.hello), "session_id", ""), &error),
         *logs = ca_strdup(json_string(JV(c.hello), "logs", ""), &error);
    pid_t pid = (pid_t)json_int(JV(c.hello), "pid", 0);
    connection_close(&c);
    wait_gone(pid);
    append_long_reply(logs);
    output = run_cli(binary, port, "--json", "RECALL", id, &code);
    CHECK(code == 0 && strstr(buf_text(&output), "session_ready") &&
              strstr(buf_text(&output), "cli_marker"),
          "CLI --resume uses saved history");
    check_history_chunks(&output);
    buf_free(&output);
    output = run_cli(binary, port, "--plain", "RECALL", id, &code);
    CHECK(code == 0 && strstr(buf_text(&output), "你> REMEMBER cli_marker") &&
              strstr(buf_text(&output), "Agent> SNAPSHOT_LONG_HEAD") &&
              strstr(buf_text(&output), "SNAPSHOT_LONG_TAIL"),
          "plain --resume prints previous conversation before continuing");
    buf_free(&output);
    free(logs);
    free(id);
    puts("PASS C client plain/verbose/JSON, exit status and --resume");
}
int main(int argc, char **argv) {
    test_init();
    CHECK(argc == 3, "server and client paths required");
    int model_port = start_mock();
    char *sessions = test_path("sessions");
    int port = start_server(argv[1], model_port, sessions, 8);
    generation_tests(port);
    session_tests(port);
    concurrent_tests(port);
    failure_tests(port);
    cli_tests(argv[2], port);
    char *limited_sessions = test_path("limited");
    int limited_port = start_server(argv[1], model_port, limited_sessions, 3);
    Connection limited = connection(limited_port);
    task(&limited, "GENERATE", "limit");
    CHECK(json_bool(last_tool(&limited, "Bash"), "ok", false),
          "round limit follows successful actual build");
    connection_close(&limited);
    free(limited_sessions);
    char *absent_sessions = test_path("absent");
    int absent_port = start_server(argv[1], reserve_port(), absent_sessions, 8);
    Connection absent = connection(absent_port);
    task(&absent, "hello", "failed");
    connection_close(&absent);
    free(absent_sessions);
    free(sessions);
    test_success = true;
    puts("All C integration tests passed.");
    return 0;
}
