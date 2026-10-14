#include "test_support.h"

static void json_tests(void) {
    Json *j = json_literal("{\"number\":4294967297,\"null\":null,\"bytes\":\"a\\u0000b\","
                           "\"offset\":\"old\",\"offset\":2}");
    CHECK(j && json_int(JV(j), "number", 0) == 4294967297LL, "64 bit integer is exact");
    CHECK(json_get(JV(j), "null") && !json_get(JV(j), "missing"), "null differs from missing");
    CHECK(json_len(json_get(JV(j), "bytes")) == 3 && json_int(JV(j), "offset", 0) == 2,
          "NUL and duplicate key behavior");
    size_t n;
    char *text = json_dump(j, false, &n);
    Json *roundtrip = json_parse(text, n);
    CHECK(roundtrip && json_equal(JV(j), JV(roundtrip)), "JSON roundtrip");
    free(text);
    json_free(roundtrip);
    Json *clone = json_clone(json_get(JV(j), "bytes"));
    json_free(j);
    CHECK(json_len(JV(clone)) == 3 && !memcmp(json_str(JV(clone)), "a\0b", 3),
          "clone owns its strings");
    json_free(clone);
    CHECK(!json_literal("{\"x\":1} trailing") && !json_literal("{\"x\":NaN}"), "strict JSON input");
    j = json_object();
    json_set_stringn(j, "text", "\xff", 1);
    text = json_dump(j, false, &n);
    roundtrip = json_parse(text, n);
    CHECK(roundtrip && !strcmp(json_string(JV(roundtrip), "text", ""), "�"),
          "raw invalid UTF-8 becomes replacement character");
    free(text);
    json_free(j);
    json_free(roundtrip);
    Buffer overflow = {.len = SIZE_MAX};
    CHECK(buf_append(&overflow, "x", 1, &error) < 0, "buffer overflow rejected before allocation");
    ca_clear_error(&error);
    puts("PASS JSON types, ownership, Unicode, integer limits");
}
static int collect_frame(void *context, const JVal *event, CaError *e) {
    (void)e;
    return json_append(context, event);
}
static int collect_sse(void *context, const char *bytes, size_t len, CaError *e) {
    Buffer *out = context;
    if (buf_append(out, bytes, len, e) < 0)
        return -1;
    return buf_append(out, "|", 1, e);
}
static void protocol_tests(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    OK(set_fd_flags(pair[0], true, &error));
    OK(set_fd_flags(pair[1], true, &error));
    Json *large = json_event("text_delta"),
         *unicode = json_literal("{\"type\":\"text_delta\",\"text\":\"你好世界\"}");
    char *payload = malloc(140000);
    CHECK(payload, "allocate large frame");
    memset(payload, 'x', 140000);
    json_set_stringn(large, "text", payload, 140000);
    free(payload);
    Buffer first = pack(JV(large)), second = pack(JV(unicode));
    pid_t child = fork();
    CHECK(child >= 0, "fork fragmented writer");
    if (!child) {
        close(pair[0]);
        raw_send(pair[1], first.data, first.len, 7);
        raw_send(pair[1], second.data, second.len, 0);
        close(pair[1]);
        _exit(0);
    }
    register_process(child);
    close(pair[1]);
    Json *received = NULL;
    CHECK(receive_frame(pair[0], &received, 5000, &error) == 1 &&
              json_equal(JV(received), JV(large)),
          "fragmented large frame");
    json_free(received);
    CHECK(receive_frame(pair[0], &received, 5000, &error) == 1 &&
              json_equal(JV(received), JV(unicode)),
          "coalesced Unicode frame");
    json_free(received);
    CHECK(receive_frame(pair[0], &received, 1000, &error) == 0, "clean EOF");
    close(pair[0]);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
          "writer exits");
    buf_free(&first);
    json_free(large);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "incremental socketpair");
    OK(set_fd_flags(pair[0], true, &error));
    FrameInbox inbox = {0};
    Json *events = json_array();
    raw_send(pair[1], second.data, 2, 0);
    int64_t start = monotonic_ms();
    CHECK(inbox_receive(&inbox, pair[0], collect_frame, events, &error) == 1 &&
              !json_len(JV(events)) && monotonic_ms() - start < 500,
          "partial frame never blocks UI");
    raw_send(pair[1], second.data + 2, second.len - 2, 0);
    raw_send(pair[1], second.data, second.len, 0);
    CHECK(inbox_receive(&inbox, pair[0], collect_frame, events, &error) == 1 &&
              json_len(JV(events)) == 2,
          "buffered frames consumed");
    close(pair[1]);
    CHECK(inbox_receive(&inbox, pair[0], collect_frame, events, &error) == 0, "incremental EOF");
    close(pair[0]);
    buf_free(&inbox.bytes);
    json_free(events);
    buf_free(&second);
    json_free(unicode);
    for (int mode = 0; mode < 3; ++mode) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "malformed socketpair");
        OK(set_fd_flags(pair[0], true, &error));
        uint32_t header = htonl(mode == 0 ? 0 : mode == 1 ? (uint32_t)CA_FRAME_LIMIT + 1 : 20);
        raw_send(pair[1], &header, 4, 0);
        if (mode == 2)
            raw_send(pair[1], "{}", 2, 0);
        close(pair[1]);
        CHECK(receive_frame(pair[0], &received, 1000, &error) < 0,
              "bad length or truncated frame rejected");
        close(pair[0]);
        ca_clear_error(&error);
    }
    const char *sse = "data: 中文\r\ndata: second\r\n\r\ndata: [DONE]\n\n";
    Buffer data = {0};
    SseParser parser = {.consume = collect_sse, .context = &data};
    for (size_t i = 0; i < strlen(sse); ++i)
        OK(sse_feed(&parser, sse + i, 1, &error));
    CHECK(!strcmp(buf_text(&data), "中文\nsecond|[DONE]|"), "SSE byte fragmentation and CRLF");
    sse_destroy(&parser);
    buf_free(&data);
    CHECK(utf8_prefix_length("\xe4\xbd", 2, false) == 0 && utf8_prefix_length("你", 3, false) == 3,
          "UTF-8 incomplete prefix held");
    puts("PASS TCP framing, incremental inbox, EOF, SSE, UTF-8");
}
static Json *execute(Tools *tools, const char *name, Json *args, const Callbacks *callbacks) {
    Json *out = NULL;
    CHECK(args, "tool arguments");
    OK(tools_execute(tools, name, JV(args), callbacks, &out, &error));
    json_free(args);
    CHECK(out, "tool result");
    return out;
}
static void expect_tool(Tools *tools, const char *name, const char *args, bool expected) {
    Json *result = execute(tools, name, json_literal(args), NULL);
    CHECK(json_bool(JV(result), "ok", false) == expected, args);
    json_free(result);
}
static Json *bash(Tools *tools, const char *command, const Callbacks *callbacks) {
    Json *args = json_object();
    json_set_string(args, "command", command);
    return execute(tools, "Bash", args, callbacks);
}
static bool cancel_after(void *context) {
    return monotonic_ms() >= *(int64_t *)context;
}
static int collect_output(void *context, const JVal *event, CaError *e) {
    JVal *text = json_get(event, "text");
    return buf_append(context, json_str(text), json_len(text), e);
}
static void template_tests(void) {
    char *source = test_path("custom-template"), *target = test_path("copied-template");
    OK(mkdir_parents(source, 0700, &error));
    char *script = path_join(source, "run.sh", &error);
    const char *code = "#!/bin/sh\nprintf 'template executed\\n'\n";
    OK(write_file_bytes(script, code, strlen(code), &error));
    CHECK(chmod(script, 0755) == 0, "make template executable");
    OK(copy_project(source, target, &error));
    char *copied = path_join(target, "run.sh", &error);
    struct stat info;
    CHECK(stat(copied, &info) == 0 && (info.st_mode & 0777) == 0755,
          "template preserves executable permission");
    CHECK(copy_project(source, target, &error) < 0,
          "template copy refuses to overwrite existing files");
    ca_clear_error(&error);
    Buffer content = read_text(copied);
    CHECK(!strcmp(buf_text(&content), code), "existing template copy remains intact");
    buf_free(&content);
    free(source);
    free(target);
    free(script);
    free(copied);
    puts("PASS executable templates and existing file preservation");
}
static void tool_tests(void) {
    char *project = test_path("tools"), *logs = test_path("logs");
    OK(copy_project(AGENT_PROJECT_DIR "/templates/c", project, &error));
    OK(mkdir_parents(logs, 0700, &error));
    Workspace workspace;
    workspace_init(&workspace);
    OK(open_workspace(project, &workspace, &error));
    Workspace another;
    workspace_init(&another);
    CHECK(open_workspace(project, &another, &error) < 0, "same workspace locked");
    ca_clear_error(&error);
    Tools tools = {0};
    OK(tools_init(&tools, project, logs, 10, &error));
    expect_tool(&tools, "Read", "{\"file_path\":\"README.md\",\"offset\":18446744073709551615}",
                false);
    expect_tool(&tools, "Read", "{\"file_path\":\"../secret\"}", false);
    expect_tool(&tools, "Read", "{\"file_path\":\"/etc/passwd\"}", false);
    expect_tool(&tools, "Write", "{\"file_path\":\".coding-agent.lock\",\"content\":\"oops\"}",
                false);
    expect_tool(&tools, "Write", "{\"file_path\":\"bad\\u0000.c\",\"content\":\"oops\"}", false);
    char *link = path_join(project, "src/link", &error);
    CHECK(symlink(test_root, link) == 0, "create symlink");
    free(link);
    expect_tool(&tools, "Write", "{\"file_path\":\"src/link/out.c\",\"content\":\"oops\"}", false);
    expect_tool(&tools, "Read", "{\"file_path\":\"README.md\",\"offset\":4294967297}", false);
    expect_tool(&tools, "Read", "{\"file_path\":\"README.md\",\"limit\":1.5}", false);
    expect_tool(&tools, "Read", "{\"file_path\":\"README.md\",\"offset\":null}", false);
    expect_tool(&tools, "Glob", "{\"pattern\":\"**/*\",\"unexpected\":1}", false);
    expect_tool(&tools, "Write",
                "{\"file_path\":\"tests/data.txt\",\"content\":\"one\\ntwo\\none\\n\"}", true);
    Json *result =
        execute(&tools, "Read",
                json_literal("{\"file_path\":\"tests/data.txt\",\"offset\":2,\"limit\":1}"), NULL);
    CHECK(!strcmp(json_string(JV(result), "content", ""), "2: two\n") &&
              json_int(JV(result), "next_offset", 0) == 3 &&
              json_bool(JV(result), "truncated", false),
          "Read pagination");
    json_free(result);
    expect_tool(
        &tools, "Edit",
        "{\"file_path\":\"tests/data.txt\",\"old_string\":\"one\",\"new_string\":\"changed\"}",
        false);
    expect_tool(&tools, "Edit",
                "{\"file_path\":\"tests/data.txt\",\"old_string\":\"missing\",\"new_string\":\"\"}",
                false);
    expect_tool(
        &tools, "Edit",
        "{\"file_path\":\"tests/data.txt\",\"old_string\":\"two\",\"new_string\":\"second\"}",
        true);
    expect_tool(
        &tools, "Edit",
        "{\"file_path\":\"tests/"
        "data.txt\",\"old_string\":\"one\",\"new_string\":\"one+one\",\"replace_all\":true}",
        true);
    expect_tool(&tools, "Write", "{\"file_path\":\"nul.txt\",\"content\":\"a\\u0000b\"}", true);
    result = execute(&tools, "Read", json_literal("{\"file_path\":\"nul.txt\"}"), NULL);
    CHECK(json_len(json_get(JV(result), "content")) == 7, "file NUL preserved");
    json_free(result);
    expect_tool(&tools, "Write", "{\"file_path\":\"root.c\",\"content\":\"\"}", true);
    expect_tool(&tools, "Write", "{\"file_path\":\"pkg/deep/test.c\",\"content\":\"\"}", true);
    result = execute(&tools, "Glob", json_literal("{\"pattern\":\"**/*.c\"}"), NULL);
    Json *files = json_literal("[\"pkg/deep/test.c\",\"root.c\"]");
    CHECK(json_equal(json_get(JV(result), "files"), JV(files)),
          "globstar covers root and nested paths");
    json_free(files);
    json_free(result);
    result = bash(&tools, "printf 'stdout\\n'; printf 'stderr\\n' >&2; exit 7", NULL);
    CHECK(json_int(JV(result), "exit_code", 0) == 7 &&
              !strcmp(json_string(JV(result), "output", ""), "stdout\nstderr\n"),
          "actual command exit and both streams");
    json_free(result);
    result = bash(&tools, "make --no-print-directory test", NULL);
    CHECK(!json_bool(JV(result), "ok", true), "empty project cannot pretend to compile");
    json_free(result);
    Buffer fixture = fixture_source();
    Json *args = json_object();
    json_set_string(args, "file_path", "src/main.c");
    json_set_stringn(args, "content", fixture.data, fixture.len);
    buf_free(&fixture);
    result = execute(&tools, "Write", args, NULL);
    CHECK(json_bool(JV(result), "ok", false), "write actual C source");
    json_free(result);
    result = bash(&tools, "make --no-print-directory test", NULL);
    CHECK(json_bool(JV(result), "ok", false) &&
              strstr(json_string(JV(result), "output", ""), "PASS ascending"),
          "actual C compilation and self-tests");
    json_free(result);
    expect_tool(
        &tools, "Write",
        "{\"file_path\":\"src/helper.c\",\"content\":\"int helper(void) { return 7; }\\n\"}", true);
    result = bash(&tools, "make --no-print-directory test", NULL);
    CHECK(json_bool(JV(result), "ok", false) &&
              strstr(json_string(JV(result), "output", ""), "src/helper.c"),
          "all generated C sources compiled");
    json_free(result);
    expect_tool(&tools, "Write", "{\"file_path\":\"src/main.c\",\"content\":\"invalid C source\"}",
                true);
    result = bash(&tools, "make --no-print-directory test", NULL);
    CHECK(!json_bool(JV(result), "ok", true), "real compiler failure");
    json_free(result);
    args = json_literal("{\"command\":\"sleep 30\",\"timeout\":0.15}");
    int64_t start = monotonic_ms();
    result = execute(&tools, "Bash", args, NULL);
    CHECK(json_bool(JV(result), "timed_out", false) && monotonic_ms() - start < 2000,
          "fractional timeout kills command");
    json_free(result);
    int64_t deadline = monotonic_ms() + 150;
    Callbacks callbacks = {.cancel = cancel_after, .cancel_ctx = &deadline};
    args = json_literal("{\"command\":\"sleep 30\"}");
    CHECK(tools_execute(&tools, "Bash", JV(args), &callbacks, &result, &error) < 0 &&
              error.code == CA_CANCELLED,
          "cancellation propagates");
    json_free(args);
    ca_clear_error(&error);
    Buffer output = {0};
    callbacks = (Callbacks){.emit = collect_output, .emit_ctx = &output};
    result =
        bash(&tools, "echo HEAD; yes padding1234567890 | head -n 1000; echo TAIL_ERROR; exit 1",
             &callbacks);
    CHECK(json_bool(JV(result), "output_truncated", false) &&
              json_len(json_get(JV(result), "output")) <= 4096 &&
              strstr(json_string(JV(result), "output", ""), "TAIL_ERROR"),
          "head/tail feedback bounded");
    char *log = path_join(logs, json_string(JV(result), "log_file", ""), &error);
    Buffer full = read_text(log);
    CHECK(full.len == output.len && !memcmp(full.data, output.data, full.len),
          "complete output retained in log");
    free(log);
    buf_free(&full);
    buf_free(&output);
    json_free(result);
    unsigned long previous = tools.commands;
    tools_destroy(&tools);
    OK(tools_init(&tools, project, logs, 10, &error));
    CHECK(tools.commands == previous, "restored tool counter retains existing logs");
    tools_destroy(&tools);
    workspace_destroy(&workspace);
    workspace_destroy(&another);
    free(project);
    free(logs);
    puts("PASS workspaces, five tools, real C builds/tests, timeout, cancellation, logs");
}
static void snapshot_tests(void) {
    char *root = test_path("snapshot"), *work = test_path("snapshot-work"),
         *log = path_join(root, "history.jsonl", &error);
    OK(mkdir_parents(root, 0700, &error));
    OK(mkdir_parents(work, 0700, &error));
    const char *history =
        "{\"message\":{\"role\":\"system\",\"content\":\"rules\"}}\n{\"message\":{\"role\":"
        "\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"orphan\",\"type\":\"function\","
        "\"function\":{\"name\":\"Bash\",\"arguments\":\"{}\"}}]}}\n{broken\n";
    OK(write_file_bytes(log, history, strlen(history), &error));
    OK(snapshot_save_workspace(root, work, &error));
    SessionSnapshot snapshot = {0};
    OK(snapshot_load(root, &snapshot, &error));
    CHECK(snapshot.messages == 3 &&
              !strcmp(json_string(json_at(JV(snapshot.history), 2), "role", ""), "tool"),
          "orphan tool result repaired and torn line skipped");
    CHECK(snapshot.workspace && snapshot_has_workspace(root), "workspace marker retained");
    Json *sessions = list_sessions(test_root, &error);
    CHECK(sessions && json_len(JV(sessions)) == 1, "only recoverable sessions listed");
    json_free(sessions);
    snapshot_destroy(&snapshot);
    FILE *file = fopen(log, "a");
    CHECK(file, "append recovered history");
    Json *answered = json_literal(
        "{\"message\":{\"role\":\"tool\",\"tool_call_id\":\"orphan\",\"content\":\"done\"}}");
    Json *repeated = json_literal("{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_"
                                  "calls\":[{\"id\":\"orphan\"}]}}");
    OK(append_json(file, JV(answered), &error));
    OK(append_json(file, JV(repeated), &error));
    fclose(file);
    json_free(answered);
    json_free(repeated);
    OK(snapshot_load(root, &snapshot, &error));
    CHECK(snapshot.messages == 5 &&
              !strcmp(json_string(json_at(JV(snapshot.history), 4), "role", ""), "tool"),
          "repeated tool ID repairs the current incomplete call");
    snapshot_destroy(&snapshot);
    free(root);
    free(work);
    free(log);
    puts("PASS session persistence and recovery");
}
int main(void) {
    test_init();
    json_tests();
    protocol_tests();
    template_tests();
    tool_tests();
    snapshot_tests();
    test_success = true;
    puts("All C core tests passed.");
    return 0;
}
