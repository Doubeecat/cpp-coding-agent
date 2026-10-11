#include "client_tui.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

using namespace ca;
static void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("FAIL: " + message);
}
static std::string read_text(const fs::path& file) {
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Temporary {
    fs::path path;
    Temporary() {
        char name[] = "/tmp/cpp-agent-tests-XXXXXX";
        char* directory = mkdtemp(name);
        if (!directory) throw std::runtime_error("mkdtemp failed");
        path = directory;
    }
    ~Temporary() {
        if (std::uncaught_exceptions()) std::cerr << "test files: " << path << '\n';
        else { std::error_code error; fs::remove_all(path, error); }
    }
};
struct Process {
    pid_t pid = -1;
    explicit Process(pid_t value) : pid(value) { setpgid(pid, pid); }
    ~Process() {
        if (pid <= 0) return;
        kill(-pid, SIGTERM);
        auto deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline) {
            int status;
            auto result = waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) return;
            usleep(10000);
        }
        kill(-pid, SIGKILL);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
};
static void raw_send(int fd, const std::string& bytes, bool fragments = false) {
    std::size_t position = 0;
    while (position < bytes.size()) {
        ssize_t sent = send(fd, bytes.data() + position,
                            fragments ? std::min<std::size_t>(7, bytes.size() - position) : bytes.size() - position, 0);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd wait{fd, POLLOUT, 0}; poll(&wait, 1, 100); continue;
        }
        if (sent <= 0) throw std::runtime_error("raw send failed");
        position += static_cast<std::size_t>(sent);
    }
}
static std::string packed(const Json& message) {
    auto text = message.dump();
    std::uint32_t size = htonl(static_cast<std::uint32_t>(text.size()));
    return std::string(reinterpret_cast<char*>(&size), 4) + text;
}
static void protocol_tests() {
    int pair[2]; check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    Fd first(pair[0]), second(pair[1]);
    set_fd_flags(first.get()); set_fd_flags(second.get());
    Json one{{"type", "中文"}, {"text", std::string(60000, 'x')}};
    Json two{{"type", "second"}, {"text", "你好🌏"}};
    pid_t child = fork(); check(child >= 0, "fork protocol writer");
    if (!child) {
        first = Fd();
        try { raw_send(second.get(), packed(one) + packed(two), true); _exit(0); }
        catch (...) { _exit(1); }
    }
    second = Fd();
    check(receive_frame(first.get(), 5000) == one, "fragmented large TCP frame");
    check(receive_frame(first.get(), 5000) == two, "coalesced UTF-8 frame");
    check(!receive_frame(first.get(), 1000), "clean EOF");
    int status; waitpid(child, &status, 0); check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "writer exits");
    for (bool truncated : {false, true}) {
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair malformed");
        Fd reader(pair[0]), writer(pair[1]); set_fd_flags(reader.get());
        std::uint32_t size = htonl(truncated ? 100 : static_cast<std::uint32_t>(frame_limit + 1));
        raw_send(writer.get(), std::string(reinterpret_cast<char*>(&size), 4) + (truncated ? "{}" : ""));
        writer = Fd();
        bool rejected = false;
        try { receive_frame(reader.get(), 1000); } catch (const std::exception&) { rejected = true; }
        check(rejected, truncated ? "truncated body rejected" : "oversize frame rejected");
    }
    std::vector<std::string> data;
    SseParser parser([&](const std::string& event) { data.push_back(event); });
    std::string events = ": ping\r\ndata: 中文\r\ndata: second\r\n\r\ndata: [DONE]\n\n";
    for (char byte : events) parser.feed(std::string(1, byte));
    check(data == std::vector<std::string>({"中文\nsecond", "[DONE]"}), "SSE byte fragmentation and CRLF");
    std::string pending = "\xe4\xbd";
    check(utf8_prefix(pending).empty(), "incomplete UTF-8 held");
    pending += "\xa0";
    check(utf8_prefix(pending) == "你" && pending.empty(), "complete UTF-8 emitted");
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "incremental socketpair");
    Fd reader(pair[0]), writer(pair[1]);
    FrameInbox inbox;
    std::vector<Json> received;
    auto consume = [&](const Json& event) { received.push_back(event); };
    auto frame = packed(two);
    raw_send(writer.get(), frame.substr(0, 2));
    auto start = Clock::now();
    check(inbox.receive(reader.get(), consume) && received.empty() && Clock::now() - start < std::chrono::milliseconds(500),
          "unfinished frame never blocks interactive input");
    raw_send(writer.get(), frame.substr(2) + frame.substr(0, 7));
    check(inbox.receive(reader.get(), consume) && received == std::vector<Json>{two}, "complete frame consumed before next fragment");
    raw_send(writer.get(), frame.substr(7) + frame);
    check(inbox.receive(reader.get(), consume) && received == std::vector<Json>({two, two, two}), "multiple buffered frames consumed");
    writer = Fd();
    check(!inbox.receive(reader.get(), consume), "interactive clean EOF");
    for (bool truncated : {false, true}) {
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "incremental malformed socketpair");
        Fd bad_reader(pair[0]), bad_writer(pair[1]);
        FrameInbox bad_inbox;
        std::uint32_t size = htonl(truncated ? 100 : static_cast<std::uint32_t>(frame_limit + 1));
        raw_send(bad_writer.get(), std::string(reinterpret_cast<char*>(&size), 4) + (truncated ? "{}" : ""));
        bad_writer = Fd();
        bool rejected = false;
        try { bad_inbox.receive(bad_reader.get(), consume); } catch (const std::exception&) { rejected = true; }
        check(rejected, "interactive malformed frames rejected");
    }
    std::cout << "PASS framing, partial TCP reads, EOF, SSE, UTF-8\n";
}
static void ui_tests() {
    UiState ui;
    ui.apply({{"type", "hello"}, {"workspace", "/tmp/我的项目"}, {"model", "qwen3.5-4b"}, {"context", 8192}});
    check(ui.tokens == -1 && ui.workspace == "/tmp/我的项目" && ui.model == "qwen3.5-4b", "footer metadata comes from server, no invented token count");
    ui.ask("生成代码", "1");
    check(ui.status == "思考中", "waiting phase is visible immediately after sending a prompt");
    usleep(20000);
    check(ui.elapsed_seconds() >= 0.01, "thinking timer advances without model events");
    ui.apply({{"type", "context_usage"}, {"context_tokens", 1234}, {"input_tokens", 1200}, {"context", 8192}});
    check(ui.tokens == 1234 && ui.context == 8192, "footer uses full measured context count");
    const std::string source = "// 完整源码\nint main() { return 0; }\n";
    ui.apply({{"type", "tool_start"}, {"name", "write_file"}, {"args", {{"path", "src/main.cpp"}, {"content", source}}}});
    int write = static_cast<int>(ui.entries.size()) - 1;
    check(!ui.entries[write].expanded && ui.summary(write).find("src/main.cpp") != std::string::npos &&
          ui.summary(write).find("int main") == std::string::npos, "source hidden in concise tool card");
    ui.toggle(write);
    check(ui.entries[write].expanded && ui.details(write).find(source) != std::string::npos, "expansion preserves complete source");
    ui.apply({{"type", "tool_result"}, {"result", {{"ok", true}, {"bytes", source.size()}}}});
    ui.apply({{"type", "tool_start"}, {"name", "build_and_test"}, {"args", Json::object()}});
    int build = static_cast<int>(ui.entries.size()) - 1;
    ui.apply({{"type", "build_output"}, {"text", "compiler error in main.cpp\n"}});
    ui.apply({{"type", "tool_result"}, {"result", {{"ok", false}, {"exit_code", 2}}}});
    check(!ui.entries[build].expanded && ui.summary(build).find("失败") != std::string::npos &&
          ui.details(build).find("compiler error") != std::string::npos && ui.details(write).find("compiler error") == std::string::npos,
          "failed build output stays attached to its collapsed card");
    ui.apply({{"type", "tool_start"}, {"name", "read_file"}, {"args", {{"path", "src/main.cpp"}}}});
    ui.apply({{"type", "task_done"}, {"status", "cancelled"}});
    check(!ui.busy && ui.summary(static_cast<int>(ui.entries.size()) - 1).find("已中止") != std::string::npos,
          "interrupted tool has a readable terminal state without a result");
    double elapsed = ui.elapsed_seconds();
    usleep(10000);
    check(ui.elapsed_seconds() == elapsed, "completed elapsed time stops advancing");
    ui.apply({{"type", "tool_result"}, {"name", "write_file"}, {"result", {{"ok", false}, {"error", "invalid argument JSON"}}}});
    int invalid = static_cast<int>(ui.entries.size()) - 1;
    check(ui.summary(invalid).find("失败") != std::string::npos && ui.details(invalid).find("invalid argument JSON") != std::string::npos,
          "argument parsing failure is visible even without a tool_start event");
    Json full_result{{"ok", true}, {"content", "第一行\n最后一行"}, {"output", "完整输出"},
                     {"nested", {{"value", "KEEP_NESTED_FIELD"}}}};
    ui.apply({{"type", "tool_result"}, {"name", "read_file"}, {"result", full_result}});
    auto details = ui.details(static_cast<int>(ui.entries.size()) - 1);
    check(details.find(full_result.dump(2)) != std::string::npos && ui.details(write).find(ui.entries[write].args.dump(2)) != std::string::npos,
          "expanded JSON retains every argument/result field, including content, output and nested data");
    std::cout << "PASS measured UI metadata, collapsed source/output cards, expansion, interrupted tools\n";
}
static void workspace_tests(const fs::path& temp) {
    Config config;
    auto directory = temp / "direct workspace";
    fs::create_directories(directory / "src");
    std::ofstream(directory / "Makefile") << "test:\n\t@echo custom\n";
    std::ofstream(directory / "README.md") << "Keep my own instructions.\n";
    std::ofstream(directory / "src/keep.hpp") << "// existing file\n";
    {
        auto workspace = open_workspace(directory, config);
        check(workspace.root == fs::canonical(directory) && read_text(directory / "Makefile").find("custom") != std::string::npos &&
              read_text(directory / "README.md") == "Keep my own instructions.\n" && read_text(directory / "src/keep.hpp") == "// existing file\n",
              "selected directory is used directly without overwriting existing files");
        bool rejected = false;
        try { auto conflict = open_workspace(directory, config); } catch (const std::exception&) { rejected = true; }
        check(rejected, "same workspace cannot be edited by two active sessions");
        auto other = open_workspace(temp / "another workspace", config);
        Tools tools(workspace.root, temp, 5);
        auto listing = tools.execute("list_files", Json::object(), [](const Json&) {}, [] { return false; });
        check(listing.dump().find(".coding-agent.lock") == std::string::npos, "workspace lock not exposed as a project file");
    }
    auto reopened = open_workspace(directory, config);
    auto custom = temp / "custom template";
    fs::create_directories(custom / "src");
    std::ofstream(custom / "Makefile") << "test:\n\t@echo custom\n";
    config.project = custom;
    auto minimal = open_workspace(temp / "custom target", config);
    check(fs::is_regular_file(minimal.root / "README.md"), "custom template without README remains supported");
    std::cout << "PASS direct workspaces, existing file preservation, concurrent locks, template initialization\n";
}
static void tui_terminal_tests(const std::string& binary, const fs::path& temp) {
    auto listener = listen_tcp("127.0.0.1", 0);
    auto port = std::to_string(socket_port(listener.get()));
    auto directory = fs::absolute(temp / "终端 project").string();
    winsize size{}; size.ws_row = 48; size.ws_col = 120;
    int master;
    pid_t child = forkpty(&master, nullptr, nullptr, &size);
    check(child >= 0, "forkpty client");
    if (!child) {
        setenv("TERM", "xterm-256color", 1);
        setenv("LANG", "en_US.UTF-8", 1);
        execl(binary.c_str(), binary.c_str(), "--port", port.c_str(), "--workdir", directory.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    Process process(child);
    Fd terminal(master); set_fd_flags(terminal.get());
    pollfd ready{listener.get(), POLLIN, 0};
    check(poll(&ready, 1, 5000) > 0, "interactive client connects");
    Fd connection(accept(listener.get(), nullptr, nullptr)); set_fd_flags(connection.get());
    send_frame(connection.get(), {{"type", "hello"}, {"workspace", "/tmp/default"}, {"model", "qwen3.5-4b"}, {"context", 8192}, {"session_id", "pty-test"}});
    auto configure = receive_frame(connection.get(), 5000);
    check(configure && configure->value("type", "") == "set_workspace" && configure->at("workspace") == directory,
          "CLI configures selected work directory before any user message");
    send_frame(connection.get(), {{"type", "workspace_ready"}, {"workspace", directory}, {"model", "qwen3.5-4b"}});
    std::string screen;
    auto drain = [&] {
        char bytes[8192];
        ssize_t count;
        while ((count = read(terminal.get(), bytes, sizeof(bytes))) > 0) screen.append(bytes, static_cast<std::size_t>(count));
    };
    auto capture = [&](const std::function<bool()>& condition, int timeout = 5000) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
        while (Clock::now() < deadline) {
            pollfd fd{terminal.get(), POLLIN, 0};
            if (poll(&fd, 1, 50) > 0) {
                drain();
            }
            if (condition()) return true;
        }
        std::cerr << "terminal bytes: " << screen.substr(screen.size() > 8192 ? screen.size() - 8192 : 0) << '\n';
        return false;
    };
    auto keys = [&](const std::string& text) {
        std::size_t at = 0;
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (at < text.size()) {
            auto count = write(terminal.get(), text.data() + at, text.size() - at);
            if (count > 0) at += static_cast<std::size_t>(count);
            else if ((errno == EAGAIN || errno == EWOULDBLOCK) && Clock::now() < deadline) {
                drain(); pollfd ready{terminal.get(), POLLIN | POLLOUT, 0}; poll(&ready, 1, 50);
            }
            else if (errno != EINTR) throw std::runtime_error("PTY input failed");
        }
    };
    auto resize = [&](int columns, int rows) {
        size.ws_col = static_cast<unsigned short>(columns); size.ws_row = static_cast<unsigned short>(rows);
        check(ioctl(terminal.get(), TIOCSWINSZ, &size) == 0, "resize PTY");
        kill(child, SIGWINCH); screen.clear();
    };
    auto submitted = [&] {
        check(capture([&] { pollfd request{connection.get(), POLLIN, 0}; return poll(&request, 1, 0) > 0; }), "client submits while terminal output is drained");
        return receive_frame(connection.get(), 5000);
    };
    auto wheel = [&](unsigned button, bool legacy, int count = 100) {
        std::string reports;
        for (int i = 0; i < count; ++i)
            if (legacy) reports += std::string("\x1b[M") + char(button + 32) + char(110 + 32) + char(5 + 32);
            else reports += "\x1b[<" + std::to_string(button) + ";3;5M";
        keys(reports);
    };
    check(capture([&] { return screen.find("Token") != std::string::npos && screen.find("qwen3.5-4b") != std::string::npos; }), "pinned footer renders model and token limit");
    check(screen.find(directory) != std::string::npos && screen.find("8192") != std::string::npos, "footer renders selected Unicode directory and per-slot capacity");
    std::string long_prompt = "PROMPT_BEGIN_5861\n" + std::string(240, 'a') + "中文需求\n" +
                              std::string(150, 'b') + "\nPROMPT_END_9786";
    keys("\x1b[200~" + long_prompt + "\x1b[201~");
    check(capture([&] { return screen.find("PROMPT_END_9786") != std::string::npos; }), "multiline paste remains in editor");
    pollfd no_submit{connection.get(), POLLIN, 0};
    check(poll(&no_submit, 1, 100) == 0, "pasted newlines never submit partial prompts");
    resize(119, 48);
    check(capture([&] { return screen.find("PROMPT_BEGIN_5861") != std::string::npos && screen.find("PROMPT_END_9786") != std::string::npos; }),
          "one terminal frame contains both beginning and end of a wrapped long prompt");
    keys("\x15");
    resize(120, 48);
    check(capture([&] { return screen.find("Token") != std::string::npos; }), "cleared editor returns to normal layout");
    keys("生成代码\n");
    auto question = submitted();
    check(question && question->at("text") == "生成代码", "wide terminal input preserves Chinese text");
    check(capture([&] { return screen.find("思考中") != std::string::npos; }), "thinking status appears before the first model event");
    auto id = question->at("request_id");
    auto event = [&](Json value) { value["request_id"] = id; send_frame(connection.get(), value); };
    event({{"type", "context_usage"}, {"context_tokens", 1234}, {"context", 8192}});
    event({{"type", "text_delta"}, {"text", "CHAT_STREAM_ONE"}});
    event({{"type", "tool_start"}, {"name", "write_file"}, {"args", {{"path", "src/main.cpp"}, {"content", "CODE_MARKER_4862\nint main() {}"}}}});
    event({{"type", "tool_result"}, {"result", {{"ok", true}, {"bytes", 40}}}});
    event({{"type", "text_delta"}, {"text", "CHAT_STREAM_TWO"}});
    event({{"type", "tool_start"}, {"name", "build_and_test"}, {"args", Json::object()}});
    event({{"type", "build_output"}, {"text", "BUILD_MARKER_9217\n"}});
    check(capture([&] { return screen.find("build_and_test") != std::string::npos; }), "streamed text and tool cards render before task completion");
    check(screen.find("CODE_MARKER_4862") == std::string::npos && screen.find("BUILD_MARKER_9217") == std::string::npos,
          "default collapsed cards do not print full source or build output");
    keys("\t\n");
    check(capture([&] { return screen.find("BUILD_MARKER_9217") != std::string::npos; }), "Tab and Enter expand selected tool while generation remains active");
    // Respond in the mouse protocol ncurses enabled. Apple's terminfo uses legacy X10.
    if (screen.find("?1006h") != std::string::npos) keys("\x1b[<0;3;8M\x1b[<0;3;8m");
    else keys(std::string("\x1b[M") + char(32) + char(35) + char(40)); // Left press at column 3, row 8.
    check(capture([&] { return screen.find("CODE_MARKER_4862") != std::string::npos; }), "mouse click expands write tool source");
    keys("\x1b");
    usleep(80000);
    keys("下一轮草稿\n");
    check(capture([&] { return screen.find("结束后再发送") != std::string::npos; }), "input stays responsive and preserves a draft during generation");
    pollfd pending{connection.get(), POLLIN, 0};
    check(poll(&pending, 1, 100) == 0, "busy client does not queue another task into current request");
    event({{"type", "build_output"}, {"text", "BUILD_STREAM_UPDATE\n"}});
    event({{"type", "tool_result"}, {"result", {{"ok", true}, {"exit_code", 0}}}});
    event({{"type", "task_done"}, {"status", "success"}});
    check(capture([&] { return screen.find("BUILD_STREAM_UPDATE") != std::string::npos; }), "expanded build details update as more output arrives");
    size.ws_row = 24; size.ws_col = 60;
    check(ioctl(terminal.get(), TIOCSWINSZ, &size) == 0, "resize PTY");
    kill(child, SIGWINCH);
    screen.clear();
    check(capture([&] { return screen.find("Token") != std::string::npos && screen.find("qwen3.5-4b") != std::string::npos; }),
          "footer survives terminal resize");
    keys("\n");
    question = submitted();
    check(question && question->at("text") == "下一轮草稿", "saved draft can be sent after task finishes");
    id = question->at("request_id");
    event({{"type", "tool_start"}, {"name", "read_file"}, {"args", {{"path", "src/main.cpp"}}}});
    event({{"type", "error"}, {"message", "INTERRUPTED_TOOL"}});
    event({{"type", "task_done"}, {"status", "failed"}});
    check(capture([&] { return screen.find("已中止") != std::string::npos; }), "interrupted tool with no result remains renderable");
    // Long results must stay complete and reachable in both mouse encodings on macOS.
    keys("\x1b"); usleep(80000);
    std::string oversized_prompt = long_prompt + "\n";
    for (int i = 0; i < 35; ++i) oversized_prompt += "EDITOR_EXTRA_" + std::to_string(i) + " 中文行\n";
    oversized_prompt += "EDITOR_BOTTOM_4285";
    keys("\x1b[200~" + oversized_prompt + "\x1b[201~");
    check(capture([&] { return screen.find("EDITOR_BOTTOM_4285") != std::string::npos; }), "oversized editor follows cursor and keeps final line visible");
    screen.clear();
    std::string editor_scroll;
    for (int i = 0; i < 40; ++i) editor_scroll += "\x1b[<64;10;10M";
    keys(editor_scroll);
    check(capture([&] { return screen.find("PROMPT_BEGIN_5861") != std::string::npos; }), "wheel can inspect beginning of an oversized multiline editor");
    keys("\n");
    question = submitted();
    check(question && question->at("text") == oversized_prompt, "multiline long prompt is submitted intact as one request");
    id = question->at("request_id");
    Json files = Json::array();
    for (int i = 0; i < 80; ++i) files.push_back("src/file_" + std::to_string(i) + ".cpp");
    files.push_back("JSON_TAIL_MARKER_6392");
    event({{"type", "tool_start"}, {"name", "list_files"}, {"args", Json::object()}});
    event({{"type", "tool_result"}, {"result", {{"ok", true}, {"files", files}, {"truncated", false},
                                                {"content", "KEEP_CONTENT_FIELD"}, {"output", "KEEP_OUTPUT_FIELD"}}}});
    event({{"type", "task_done"}, {"status", "success"}});
    check(capture([&] { return screen.find("list_files") != std::string::npos; }), "new tool is rendered before keyboard selection");
    screen.clear();
    keys("\t\n");
    check(capture([&] { return screen.find("参数 JSON") != std::string::npos || screen.find("▾ list_files") != std::string::npos; }),
          "long tool result expands without forcing its tail into view");
    check(screen.find("JSON_TAIL_MARKER_6392") == std::string::npos, "long result tail initially lies below viewport");
    wheel(65, false);
    check(capture([&] { return screen.find("JSON_TAIL_MARKER_6392") != std::string::npos; }), "SGR wheel-down reaches end of full JSON result");
    wheel(64, false);
    check(capture([&] { return screen.find("CHAT_STREAM_ONE") != std::string::npos; }), "SGR wheel-up reaches previous messages");
    resize(120, 24);
    check(capture([&] { return screen.find("CHAT_STREAM_ONE") != std::string::npos; }), "scroll position survives resize before testing legacy coordinates");
    screen.clear();
    wheel(65, true);
    check(capture([&] { return screen.find("JSON_TAIL_MARKER_6392") != std::string::npos && screen.find("KEEP_OUTPUT_FIELD") != std::string::npos; }),
          "legacy wheel-down with non-UTF8 coordinate bytes reaches complete JSON fields");
    screen.clear();
    wheel(64, true);
    check(capture([&] { return screen.find("CHAT_STREAM_ONE") != std::string::npos; }), "legacy wheel-up reaches previous messages");
    screen.clear();
    keys("\x1b[<65;3;"); usleep(20000); keys("5M");
    wheel(65, false);
    check(capture([&] { return screen.find("JSON_TAIL_MARKER_6392") != std::string::npos; }), "fragmented mouse report resumes without freezing input");
    keys("\x1b"); usleep(80000);
    keys("first\x1b\rsecond\x0ethird\r");
    question = submitted();
    check(question && question->at("text") == "first\nsecond\nthird", "Alt+Enter and Ctrl+N insert line breaks without submitting");
    id = question->at("request_id");
    event({{"type", "task_done"}, {"status", "success"}});
    check(capture([&] { return screen.find("first") != std::string::npos; }), "multiline manual input remains visible in conversation");
    keys("\x03");
    int status = 0;
    bool exited = false;
    capture([&] {
        if (waitpid(child, &status, WNOHANG) == child) { exited = true; process.pid = -1; }
        return exited;
    });
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 1)
        std::cerr << "PTY exit: exited=" << exited << " status=" << status << " bytes=" << screen << '\n';
    check(exited && WIFEXITED(status) && WEXITSTATUS(status) == 1, "Ctrl+C exits TUI and retains failure exit code");
    termios attributes{};
    check(tcgetattr(terminal.get(), &attributes) == 0 && (attributes.c_lflag & ICANON) && (attributes.c_lflag & ECHO),
          "terminal input modes restored on exit");
    std::cout << "PASS real PTY: multiline prompt/paste, complete JSON, SGR/legacy scrolling, timed status, keyboard/mouse expansion, draft, resize, terminal restoration\n";
}
static const std::string good_source = R"(#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
std::vector<int> sorted(std::vector<int> values) {
    std::sort(values.begin(), values.end());
    return values;
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        int failures = 0;
        auto check = [&](bool ok, const std::string& name) {
            std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
            if (!ok) ++failures;
        };
        check(sorted({3, -1, 3, 0}) == std::vector<int>({-1, 0, 3, 3}), "ascending with duplicates");
        check(sorted({}).empty(), "empty input");
        std::ofstream output("build/self-test.txt");
        output << "generated program"; output.close();
        std::ifstream input("build/self-test.txt"); std::string text; std::getline(input, text);
        check(text == "generated program", "file roundtrip");
        return failures ? 1 : 0;
    }
    std::vector<int> values;
    int value;
    while (std::cin >> value) values.push_back(value);
    for (int value : sorted(values)) std::cout << value << ' ';
    std::cout << '\n';
}
)";
static const std::string extended_source = [] {
    auto source = good_source;
    const std::string original = "    for (int value : sorted(values))";
    source.replace(source.find(original), original.size(),
        "    auto result = sorted(values);\n"
        "    if (argc == 2 && std::string(argv[1]) == \"--descending\") std::reverse(result.begin(), result.end());\n"
        "    for (int value : result)");
    auto position = source.find("        std::ofstream output");
    source.insert(position,
        "        auto descending = sorted({3, -1, 0}); std::reverse(descending.begin(), descending.end());\n"
        "        check(descending == std::vector<int>({3, 0, -1}), \"descending order\");\n");
    return source;
}();
static void tool_tests(const fs::path& temp) {
    auto project = temp / "tools";
    copy_project(fs::path(AGENT_PROJECT_DIR) / "templates/cpp", project);
    Tools tools(project, temp, 10);
    auto emit = [](const Json&) {};
    auto cancel = [] { return false; };
    for (const auto& path : {"../secret", "/etc/passwd", "src/../../secret"})
        check(!tools.execute("read_file", {{"path", path}, {"start", 1}, {"count", 100}}, emit, cancel)["ok"], "path rejected");
    fs::create_symlink(temp, project / "src/link");
    check(!tools.execute("write_file", {{"path", "src/link/outside.cpp"}, {"content", "oops"}}, emit, cancel)["ok"], "symlink write rejected");
    check(!tools.execute("write_file", {{"path", "Makefile"}, {"content", "oops"}}, emit, cancel)["ok"], "build rules protected");
    check(!tools.execute("write_file", {{"path", "tests/test.cpp"}, {"content", "oops"}}, emit, cancel)["ok"], "external tests protected");
    check(!tools.execute("read_file", {{"path", "README.md"}, {"start", -1}, {"count", 5}}, emit, cancel)["ok"], "bad line range");
    check(!tools.execute("list_files", {{"unexpected", 1}}, emit, cancel)["ok"], "unexpected argument rejected");
    auto failed = tools.execute("build_and_test", Json::object(), emit, cancel);
    check(!failed["ok"] && failed["exit_code"] != 0 && !tools.tests_passed, "empty project cannot pretend to build");
    auto failing = tools.execute("write_file", {{"path", "src/main.cpp"},
        {"content", "#include <iostream>\nint main() { std::cout << \"FAIL generated check\\n\"; return 1; }\n"}}, emit, cancel);
    check(failing["ok"], "create source in initially empty project");
    failed = tools.execute("build_and_test", Json::object(), emit, cancel);
    check(!failed["ok"] && failed["exit_code"] != 0 && !tools.tests_passed &&
          failed["output"].get<std::string>().find("FAIL generated check") != std::string::npos, "real generated self-test failure");
    auto write = tools.execute("write_file", {{"path", "src/main.cpp"}, {"content", good_source}}, emit, cancel);
    check(write["ok"] && tools.dirty && !tools.tests_passed, "write marks unverified");
    auto passed = tools.execute("build_and_test", Json::object(), emit, cancel);
    check(passed["ok"] && tools.tests_passed && !tools.dirty, "actual generated code compiles and self-tests pass");
    check(passed["output"].get<std::string>().find("PASS ascending with duplicates") != std::string::npos, "real feature test executed");
    check(read_text(project / "build/self-test.txt") == "generated program", "generated program reads and writes a real file");
    auto extra = tools.execute("write_file", {{"path", "src/helper.cpp"}, {"content", "int helper() { return 7; }\n"}}, emit, cancel);
    check(extra["ok"] && tools.dirty, "additional source file created");
    passed = tools.execute("build_and_test", Json::object(), emit, cancel);
    check(passed["ok"] && passed["output"].get<std::string>().find("src/helper.cpp") != std::string::npos, "all generated source files compiled");
    tools.execute("write_file", {{"path", "src/main.cpp"}, {"content", "this is not C++"}}, emit, cancel);
    check(!tools.tests_passed && tools.dirty, "new edit invalidates old test result");
    auto bad = tools.execute("build_and_test", Json::object(), emit, cancel);
    check(!bad["ok"] && bad["exit_code"] != 0, "actual compilation error");
    auto slow = temp / "slow";
    fs::create_directories(slow / "src");
    std::ofstream(slow / "Makefile") << "test:\n\t@echo waiting; sleep 30\n";
    Tools timeout(slow, temp, 1);
    auto start = Clock::now();
    auto timed = timeout.execute("build_and_test", Json::object(), emit, cancel);
    check(timed["timed_out"] && !timed["ok"] && Clock::now() - start < std::chrono::seconds(4), "build timeout enforced");
    Tools interrupted(slow, temp, 10);
    bool cancelled = false;
    try {
        interrupted.execute("build_and_test", Json::object(), emit,
                            [&] { return Clock::now() - start > std::chrono::milliseconds(1200); });
    } catch (const Cancelled&) { cancelled = true; }
    check(cancelled, "build cancellation propagated");
    std::ofstream(slow / "Makefile") << "test:\n\t@echo HEAD; yes padding1234567890 | head -n 1000; echo TAIL_ERROR; exit 1\n";
    Tools clipped(slow, temp, 5);
    auto large = clipped.execute("build_and_test", Json::object(), emit, cancel);
    auto feedback = large["output"].get<std::string>();
    check(large["output_truncated"] && feedback.size() <= 4096 && feedback.find("HEAD") != std::string::npos &&
          feedback.find("TAIL_ERROR") != std::string::npos, "clipped output preserves beginning and error at the end");
    std::cout << "PASS file boundaries, atomic writes, real builds/tests, timeout, cancellation\n";
}
struct HttpRequest { std::string path; Json body; };
static HttpRequest read_http(int fd) {
    std::string request;
    std::size_t split = std::string::npos, length = 0;
    auto deadline = Clock::now() + std::chrono::seconds(5);
    while (true) {
        pollfd p{fd, POLLIN, 0}; poll(&p, 1, 100);
        if (Clock::now() > deadline) throw std::runtime_error("HTTP read timeout");
        char bytes[4096]; ssize_t got = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (got <= 0) throw std::runtime_error("HTTP peer closed");
        request.append(bytes, got);
        if (request.size() > frame_limit) throw std::runtime_error("HTTP request too large");
        if (split == std::string::npos && (split = request.find("\r\n\r\n")) != std::string::npos) {
            auto pos = request.find("Content-Length:");
            if (pos != std::string::npos) length = std::stoul(request.substr(pos + 15));
        }
        if (split != std::string::npos && request.size() >= split + 4 + length) break;
    }
    auto begin = request.find(' '), end = request.find(' ', begin + 1);
    return {request.substr(begin + 1, end - begin - 1),
            length ? Json::parse(request.substr(split + 4, length)) : Json::object()};
}
static void json_response(int fd, const Json& value) {
    auto text = value.dump();
    raw_send(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(text.size()) + "\r\n\r\n" + text);
}
static void sse(int fd, const Json& event) {
    raw_send(fd, "data: " + event.dump() + "\r\n\r\n", true);
}
static void delta(int fd, const Json& value, const Json& reason = nullptr) {
    sse(fd, {{"choices", Json::array({{{"index", 0}, {"delta", value}, {"finish_reason", reason}}})}});
}
static void text_reply(int fd, const std::string& text, int repeats = 1, int delay_us = 0, bool usage = false) {
    for (int i = 0; i < repeats; ++i) {
        delta(fd, {{"content", text}});
        if (delay_us) usleep(delay_us);
    }
    delta(fd, Json::object(), "stop");
    if (usage) sse(fd, {{"choices", Json::array()}, {"usage", {{"prompt_tokens", 512}, {"completion_tokens", 7}}}});
    raw_send(fd, "data: [DONE]\n\n");
}
static void tool_reply(int fd, const std::string& name, const Json& args, bool truncated = false) {
    auto argument = args.dump(-1, ' ', true);
    auto mid = argument.size() / 2;
    delta(fd, {{"content", "正在处理。"}});
    delta(fd, {{"tool_calls", Json::array({{{"index", 0}, {"id", "call_test"}, {"type", "function"},
        {"function", {{"name", name}, {"arguments", argument.substr(0, mid)}}}}})}});
    delta(fd, {{"tool_calls", Json::array({{{"index", 0}, {"function", {{"arguments", argument.substr(mid)}}}}})}});
    delta(fd, Json::object(), truncated ? "length" : "tool_calls");
    raw_send(fd, "data: [DONE]\n\n");
}
static void mock_connection(int fd, const fs::path& logs) {
    auto request = read_http(fd);
    if (request.path == "/props") { json_response(fd, {{"total_slots", 2}}); return; }
    if (request.path == "/slots") { json_response(fd, Json::array({{{"n_ctx", 8192}}, {{"n_ctx", 8192}}})); return; }
    check(request.body.contains("tools") && request.body.contains("messages"), "full messages and tools sent");
    const auto& messages = request.body.at("messages");
    std::size_t last_user = 0;
    for (std::size_t i = 0; i < messages.size(); ++i) if (messages[i].value("role", "") == "user") last_user = i;
    std::string prompt = messages[last_user].at("content").get<std::string>();
    if (request.path == "/v1/chat/completions/input_tokens") {
        int completed = 0;
        for (std::size_t i = last_user + 1; i < messages.size(); ++i) if (messages[i].value("role", "") == "tool") ++completed;
        json_response(fd, {{"input_tokens", prompt == "COUNT_OVER" || (prompt == "GENERATE_OVER_COUNT" && completed >= 3) ? 9000 : 512}});
        return;
    }
    std::ofstream(logs / (std::to_string(getpid()) + ".json")) << request.body.dump();
    raw_send(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
    int tools = 0;
    for (std::size_t i = last_user + 1; i < messages.size(); ++i) if (messages[i].value("role", "") == "tool") ++tools;
    if (prompt == "GENERATE" || prompt == "GENERATE_OVER_COUNT" || prompt == "EXTEND") {
        if (tools == 0) tool_reply(fd, "read_file", {{"path", prompt == "EXTEND" ? "src/main.cpp" : "README.md"}, {"start", 1}, {"count", 100}});
        else if (tools == 1) tool_reply(fd, "write_file", {{"path", "src/main.cpp"}, {"content", prompt == "EXTEND" ? extended_source : good_source}});
        else if (tools == 2) tool_reply(fd, "build_and_test", Json::object());
        else text_reply(fd, "已修复并通过测试。", 3, 10000);
    } else if (prompt == "UNVERIFIED") {
        if (!tools) tool_reply(fd, "write_file", {{"path", "src/main.cpp"}, {"content", good_source}});
        else text_reply(fd, "我声称已完成，但没有运行测试。");
    } else if (prompt == "TRUNCATED") tool_reply(fd, "write_file", {{"path", "src/main.cpp"}, {"content", good_source}}, true);
    else if (prompt == "LOOP") tool_reply(fd, "list_files", Json::object());
    else if (prompt == "RECALL") {
        std::string marker = "missing";
        for (std::size_t i = 0; i < last_user; ++i) if (messages[i].value("role", "") == "user") {
            auto earlier = messages[i].at("content").get<std::string>();
            if (earlier.rfind("REMEMBER ", 0) == 0) marker = earlier.substr(9);
        }
        text_reply(fd, marker, 4, 10000);
    } else if (prompt == "USAGE") text_reply(fd, "token statistics", 1, 0, true);
    else text_reply(fd, prompt, (prompt == "DISCONNECT" || prompt == "SURVIVE") ? 60 : 8, 30000);
}
static std::unique_ptr<Process> start_mock(int& port, const fs::path& logs) {
    fs::create_directories(logs);
    auto listener = listen_tcp("127.0.0.1", 0);
    port = socket_port(listener.get());
    pid_t child = fork(); check(child >= 0, "fork mock");
    if (!child) {
        setpgid(0, 0); signal(SIGTERM, SIG_DFL);
        while (true) {
            while (waitpid(-1, nullptr, WNOHANG) > 0) {}
            pollfd p{listener.get(), POLLIN, 0}; poll(&p, 1, 100);
            int fd = accept(listener.get(), nullptr, nullptr);
            if (fd < 0) continue;
            pid_t request = fork();
            if (!request) {
                listener = Fd();
                try { mock_connection(fd, logs); close(fd); _exit(0); }
                catch (...) { close(fd); _exit(1); }
            }
            close(fd);
        }
    }
    return std::make_unique<Process>(child);
}
static std::unique_ptr<Process> start_server(const std::string& binary, int& port, int model_port, const fs::path& sessions, int rounds = 8) {
    auto temporary = listen_tcp("127.0.0.1", 0);
    port = socket_port(temporary.get()); temporary = Fd();
    pid_t child = fork(); check(child >= 0, "fork server");
    if (!child) {
        setpgid(0, 0);
        auto p = std::to_string(port), url = "http://127.0.0.1:" + std::to_string(model_port), limit = std::to_string(rounds);
        execl(binary.c_str(), binary.c_str(), "--port", p.c_str(), "--model-url", url.c_str(),
              "--sessions", sessions.c_str(), "--max-rounds", limit.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    auto process = std::make_unique<Process>(child);
    for (int i = 0; i < 60; ++i) {
        try { auto probe = connect_tcp("127.0.0.1", port); return process; }
        catch (...) { usleep(50000); }
    }
    throw std::runtime_error("server failed to start");
}
struct Connection {
    Fd socket;
    Json hello;
    int sequence = 0;
    explicit Connection(int port) : socket(connect_tcp("127.0.0.1", port)) {
        auto value = receive_frame(socket.get(), 5000);
        check(value && value->value("type", "") == "hello", "hello received"); hello = *value;
    }
    Json configure(const fs::path& workspace) {
        send_frame(socket.get(), {{"type", "set_workspace"}, {"workspace", workspace.string()}});
        auto event = receive_frame(socket.get(), 5000);
        check(event.has_value(), "workspace response received");
        return *event;
    }
    void ask(const std::string& text) {
        send_frame(socket.get(), {{"type", "user_message"}, {"request_id", std::to_string(++sequence)}, {"text", text}});
    }
    std::vector<Json> collect() {
        std::vector<Json> events;
        while (true) {
            auto event = receive_frame(socket.get(), 15000);
            check(event.has_value(), "response before EOF");
            check(event->value("request_id", "") == std::to_string(sequence), "matching request ID");
            events.push_back(*event);
            if (event->value("type", "") == "task_done") return events;
        }
    }
};
static std::string answer(const std::vector<Json>& events) {
    std::string result;
    for (const auto& event : events) if (event.value("type", "") == "text_delta") result += event.value("text", "");
    return result;
}
static void integration_tests(const std::string& binary, const fs::path& root) {
    int model_port, port;
    auto mock = start_mock(model_port, root / "mock");
    auto server = start_server(binary, port, model_port, root / "sessions");
    {
        Connection direct(port), another(port);
        auto directory = root / "chosen project";
        check(direct.configure(directory)["type"] == "workspace_ready", "client can select a direct work directory");
        check(another.configure(directory)["type"] == "workspace_error", "concurrent client rejected for same work directory");
        check(another.configure(root / "other chosen project")["type"] == "workspace_ready", "other client can select its own directory");
        direct.ask("GENERATE"); auto generated = direct.collect();
        check(generated.back()["status"] == "success" && read_text(directory / "src/main.cpp") == good_source &&
              !fs::exists(fs::path(direct.hello["workspace"].get<std::string>()) / "src/main.cpp"), "generated source lands directly in selected directory");
        check(fs::exists(fs::path(direct.hello["logs"].get<std::string>()) / "build-1.log"), "selected directory keeps session-specific logs separate");
        check(direct.configure(root / "too late")["type"] == "workspace_error", "directory cannot change after a task starts");
        auto pid = direct.hello["pid"].get<pid_t>();
        direct.socket = Fd();
        for (int i = 0; i < 100 && kill(pid, 0) == 0; ++i) usleep(20000);
        Connection reopened(port);
        check(reopened.configure(directory)["type"] == "workspace_ready" && read_text(directory / "src/main.cpp") == good_source,
              "disconnect releases directory lock and retains generated code");
    }
    {
        Connection usage(port); usage.ask("USAGE"); auto events = usage.collect();
        int last = -1;
        for (const auto& event : events) if (event.value("type", "") == "context_usage") last = event.at("context_tokens").get<int>();
        check(last == 519, "stream usage includes both actual prompt and completion tokens");
    }
    Connection a(port), b(port);
    check(a.hello["session_id"] != b.hello["session_id"] && a.hello["pid"] != b.hello["pid"], "separate session IDs and processes");
    a.ask("REMEMBER alpha"); b.ask("REMEMBER beta");
    std::vector<Json> events[2]; bool done[2] = {false, false}; int deltas[2] = {0, 0};
    while (!done[0] || !done[1]) {
        pollfd fds[2] = {{done[0] ? -1 : a.socket.get(), POLLIN, 0}, {done[1] ? -1 : b.socket.get(), POLLIN, 0}};
        check(poll(fds, 2, 5000) > 0, "two-client responses arrive");
        for (int i = 0; i < 2; ++i) if (fds[i].revents & POLLIN) {
            auto event = receive_frame(fds[i].fd, 5000); check(event.has_value(), "stream remains open");
            events[i].push_back(*event);
            if (event->value("type", "") == "text_delta") ++deltas[i];
            if (event->value("type", "") == "task_done") {
                check(deltas[0] > 1 && deltas[1] > 1, "both streams start before either finishes");
                done[i] = true;
            }
        }
    }
    a.ask("RECALL"); b.ask("RECALL");
    auto recalled_a = a.collect(), recalled_b = b.collect();
    check(answer(recalled_a).find("alpha") != std::string::npos && answer(recalled_a).find("beta") == std::string::npos, "A context isolated");
    check(answer(recalled_b).find("beta") != std::string::npos && answer(recalled_b).find("alpha") == std::string::npos, "B context isolated");
    auto file_a = fs::path(a.hello["workspace"].get<std::string>()) / "src/main.cpp";
    auto file_b = fs::path(b.hello["workspace"].get<std::string>()) / "src/main.cpp";
    check(!fs::exists(file_a) && !fs::exists(file_b), "new sessions start without application sources");
    a.ask("GENERATE"); auto edited = a.collect();
    check(edited.back()["status"] == "success" && edited.back()["tests_passed"], "full native tool loop succeeds");
    check(read_text(file_a) == good_source && !fs::exists(file_b), "generated code workspace isolation");
    check(fs::exists(file_a.parent_path().parent_path().parent_path() / "build-1.log"), "per-session build log exists");
    a.ask("EXTEND"); auto extended = a.collect();
    check(extended.back()["status"] == "success" && read_text(file_a) == extended_source && !fs::exists(file_b),
          "same connection extends the generated program without affecting B");
    {
        Connection over_count(port); over_count.ask("GENERATE_OVER_COUNT"); auto result = over_count.collect();
        check(result.back()["status"] == "success" && result.back()["tests_passed"], "verified code can finish without over-budget model summary");
    }
    {
        Connection unverified(port); unverified.ask("UNVERIFIED"); auto result = unverified.collect();
        check(result.back()["status"] == "failed" && result.back()["unverified_changes"], "model cannot fake success without tests");
    }
    {
        Connection cut(port); auto file = fs::path(cut.hello["workspace"].get<std::string>()) / "src/main.cpp";
        cut.ask("TRUNCATED"); auto result = cut.collect();
        check(result.back()["status"] == "failed" && !fs::exists(file), "truncated tool output never creates a file");
        auto history = read_text(fs::path(cut.hello["workspace"].get<std::string>()).parent_path() / "history.jsonl");
        check(history.find("正在处理") != std::string::npos, "visible partial answer retained after interruption");
        cut.ask("COUNT_OVER"); result = cut.collect();
        check(result.back()["status"] == "context_limit", "token budget enforced before generation");
        cut.ask("LOOP"); result = cut.collect();
        check(result.back()["status"] == "limit", "agent loop bounded");
    }
    {
        int limited_port;
        auto limited_server = start_server(binary, limited_port, model_port, root / "limited", 3);
        Connection limited(limited_port); limited.ask("GENERATE"); auto result = limited.collect();
        check(result.back()["status"] == "success" && result.back()["tests_passed"] && result.back()["rounds"] == 3,
              "tests passing on the final allowed round still succeed");
    }
    {
        Connection disconnect(port), survive(port);
        auto disconnected_pid = disconnect.hello["pid"].get<pid_t>();
        disconnect.ask("DISCONNECT"); survive.ask("SURVIVE");
        while (true) {
            auto event = receive_frame(disconnect.socket.get(), 5000); check(event.has_value(), "A started before disconnect");
            if (event->value("type", "") == "text_delta") break;
        }
        disconnect.socket = Fd();
        auto result = survive.collect();
        check(result.back()["status"] == "success" && answer(result).find("SURVIVE") != std::string::npos, "B continues after A disconnects");
        bool gone = false;
        for (int i = 0; i < 100; ++i) { if (kill(disconnected_pid, 0) < 0 && errno == ESRCH) { gone = true; break; } usleep(20000); }
        check(gone, "disconnected session process reaped");
    }
    // No model process is listening on this freshly reserved port.
    auto unused = listen_tcp("127.0.0.1", 0);
    int absent = socket_port(unused.get()); unused = Fd();
    int failed_port;
    auto without_model = start_server(binary, failed_port, absent, root / "without-model");
    Connection failure(failed_port); failure.ask("hello"); auto result = failure.collect();
    check(result.back()["status"] == "failed", "model unavailable reported");
    std::cout << "PASS two-client streams, history/process/file/log isolation, disconnect, limits, truncated tools, unavailable model\n";
}
int main(int argc, char** argv) {
    try {
        check(argc == 3, "server and client binary arguments");
        signal(SIGPIPE, SIG_IGN);
        Temporary temp;
        protocol_tests();
        ui_tests();
        workspace_tests(temp.path);
        tui_terminal_tests(argv[2], temp.path);
        tool_tests(temp.path);
        Config invalid; invalid.model_url = "https://example.com";
        bool rejected = false;
        try { ModelClient model(invalid); } catch (const std::exception&) { rejected = true; }
        check(rejected, "external model URLs prohibited");
        integration_tests(argv[1], temp.path);
        std::cout << "All C++ tests passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
