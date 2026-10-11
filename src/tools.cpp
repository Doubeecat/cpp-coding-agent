#include "common.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ca {
Json tool_schema() {
    auto tool = [](const std::string& name, const std::string& description, Json properties, Json required) {
        return Json{{"type", "function"}, {"function", {
            {"name", name}, {"description", description}, {"parameters", {
                {"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}}}}};
    };
    return Json::array({
        tool("list_files", "List relative paths in this session's project; excludes build outputs.", Json::object(), Json::array()),
        tool("read_file", "Read numbered lines of a project file. Use relative paths. At most 100 lines / 4096 bytes per call.",
             {{"path", {{"type", "string"}}}, {"start", {{"type", "integer"}, {"minimum", 1}}},
              {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}}, Json::array({"path", "start", "count"})),
        tool("write_file", "Create or replace a complete source file under src/. Content must be raw C++ source, with no markdown fences. Build rules and external tests are read-only.",
             {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}, Json::array({"path", "content"})),
        tool("build_and_test", "Run the project's fixed make test command. The default template compiles src/*.cpp and runs build/app --self-test. Returns actual exit code and output. Always use after your last source modification.",
             Json::object(), Json::array())
    });
}
void copy_project(const fs::path& source, const fs::path& destination) {
    if (!fs::is_directory(source) || !fs::is_regular_file(source / "Makefile") || !fs::is_directory(source / "src"))
        throw std::runtime_error("项目模板需要 Makefile 和 src/ 目录");
    fs::create_directories(destination);
    std::size_t count = 0, total = 0;
    for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator(); ++it) {
        auto relative = it->path().lexically_relative(source);
        auto name = it->path().filename().string();
        if (name == "build" || name == ".git" || name == ".coding-agent.lock") { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (it->is_symlink()) throw std::runtime_error("项目模板不能包含符号链接");
        if (it.depth() > 8 || ++count > 128) throw std::runtime_error("项目模板过大");
        auto target = destination / relative;
        if (it->is_directory()) fs::create_directories(target);
        else if (it->is_regular_file()) {
            auto size = it->file_size();
            total += size;
            if (size > frame_limit || total > 4 * frame_limit) throw std::runtime_error("项目模板文件过大");
            fs::copy_file(it->path(), target);
        } else throw std::runtime_error("项目模板只支持普通文件和目录");
    }
}
Workspace open_workspace(const fs::path& path, const Config& config) {
    if (path.empty() || path.string().find('\0') != std::string::npos)
        throw std::runtime_error("工作目录不能为空或包含 NUL");
    fs::create_directories(path);
    auto root = fs::canonical(path);
    Fd lock(open((root / ".coding-agent.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.get() < 0) throw std::runtime_error("无法创建工作目录锁");
    if (flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
        throw std::runtime_error("工作目录已被另一个会话使用，请选择其他目录");
    for (const auto* name : {"Makefile", "README.md"}) {
        auto target = root / name;
        if (fs::is_symlink(fs::symlink_status(target))) throw std::runtime_error("工作目录的构建规则和说明不能为符号链接");
        if (!fs::exists(target)) {
            auto source = config.project / name;
            if (fs::is_regular_file(source)) fs::copy_file(source, target);
            else if (std::string(name) == "README.md") {
                std::ofstream readme(target);
                readme << "# C++ project\n\nOnly src/ is writable. Read Makefile for the build contract.\n"
                          "Run make test after every source change; success requires exit code 0.\n";
                if (!readme) throw std::runtime_error("创建项目说明失败");
            } else throw std::runtime_error("项目模板缺少 Makefile");
        }
        if (!fs::is_regular_file(target)) throw std::runtime_error(std::string(name) + " 必须为普通文件");
    }
    if (fs::is_symlink(fs::symlink_status(root / "src"))) throw std::runtime_error("src/ 不能为符号链接");
    fs::create_directories(root / "src");
    return {root, std::move(lock)};
}
Tools::Tools(fs::path workspace, fs::path logs, int timeout)
    : root_(fs::canonical(workspace)), logs_(std::move(logs)), timeout_(timeout) {}
fs::path Tools::checked_path(const std::string& value, bool write) const {
    if (value.empty() || value.find('\0') != std::string::npos) throw std::runtime_error("文件路径为空或包含 NUL");
    fs::path path(value);
    if (path.is_absolute()) throw std::runtime_error("只允许相对文件路径");
    for (const auto& part : path) if (part == "..") throw std::runtime_error("不允许 .. 路径");
    path = path.lexically_normal();
    if (write && (path.begin() == path.end() || *path.begin() != "src" || path == "src"))
        throw std::runtime_error("只允许修改 src/ 中的文件，构建规则与外部验收测试只读");
    auto checked = root_;
    for (const auto& part : path) {
        checked /= part;
        if (fs::is_symlink(fs::symlink_status(checked))) throw std::runtime_error("不允许访问符号链接");
    }
    return checked;
}
Json Tools::execute(const std::string& name, const Json& args, const Emit& emit, const Cancel& cancel) {
    try {
        if (cancel()) throw Cancelled("任务取消或超时");
        if (!args.is_object()) throw std::runtime_error("工具参数必须为 JSON 对象");
        std::vector<std::string> allowed;
        if (name == "read_file") allowed = {"path", "start", "count"};
        else if (name == "write_file") allowed = {"path", "content"};
        else if (name != "list_files" && name != "build_and_test") throw std::runtime_error("未知工具：" + name);
        for (auto it = args.begin(); it != args.end(); ++it)
            if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) throw std::runtime_error("未知参数：" + it.key());
        if (name == "list_files") {
            Json files = Json::array();
            bool truncated = false;
            for (auto it = fs::recursive_directory_iterator(root_); it != fs::recursive_directory_iterator(); ++it) {
                auto name = it->path().filename();
                if (name == "build" || name == ".git" || name == ".coding-agent.lock") {
                    if (it->is_directory()) it.disable_recursion_pending();
                    continue;
                }
                if (it->is_symlink()) continue;
                if (it.depth() > 8) { it.disable_recursion_pending(); truncated = true; continue; }
                if (it->is_regular_file()) {
                    if (files.size() == 100) { truncated = true; break; }
                    files.push_back(it->path().lexically_relative(root_).generic_string());
                }
            }
            std::sort(files.begin(), files.end());
            return {{"ok", true}, {"files", files}, {"truncated", truncated}};
        }
        if (name == "read_file") {
            std::string path = args.at("path").get<std::string>();
            int start = args.at("start").get<int>(), count = args.at("count").get<int>();
            if (!args["start"].is_number_integer() || !args["count"].is_number_integer() || start < 1 || count < 1 || count > 100)
                throw std::runtime_error("start 从 1 开始，count 必须为 1 到 100 的整数");
            auto file = checked_path(path, false);
            if (!fs::is_regular_file(file) || fs::file_size(file) > frame_limit) throw std::runtime_error("文件不存在、不是普通文件或超过 1 MiB");
            std::ifstream input(file);
            if (!input) throw std::runtime_error("打开文件失败");
            std::string line, text;
            int number = 0, last = start - 1;
            bool truncated = false;
            while (std::getline(input, line)) {
                if (++number < start) continue;
                if (number >= start + count) { truncated = true; break; }
                std::string entry = std::to_string(number) + ": " + line + '\n';
                if (text.size() + entry.size() > 4096) { truncated = true; break; }
                text += entry;
                last = number;
            }
            return {{"ok", true}, {"path", path}, {"content", text}, {"truncated", truncated}, {"next_start", last + 1}};
        }
        if (name == "write_file") {
            auto path = checked_path(args.at("path").get<std::string>(), true);
            auto content = args.at("content").get<std::string>();
            if (content.size() > 64 * 1024) throw std::runtime_error("源文件超过 64 KiB");
            fs::create_directories(path.parent_path());
            auto temp = path.string() + ".tmp-" + std::to_string(getpid());
            Fd file(open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
            if (file.get() < 0) throw std::runtime_error("创建临时文件失败");
            try {
                std::size_t offset = 0;
                while (offset < content.size()) {
                    auto bytes = write(file.get(), content.data() + offset, content.size() - offset);
                    if (bytes < 0 && errno == EINTR) continue;
                    if (bytes <= 0) throw std::runtime_error("写源文件失败");
                    offset += static_cast<std::size_t>(bytes);
                }
                if (fsync(file.get()) < 0) throw std::runtime_error("保存源文件失败");
                fs::rename(temp, path);
            } catch (...) { fs::remove(temp); throw; }
            dirty = true;
            tests_passed = false;
            return {{"ok", true}, {"path", args["path"]}, {"bytes", content.size()}};
        }
        return build(emit, cancel);
    } catch (const Cancelled&) { throw; }
      catch (const std::exception& error) { return {{"ok", false}, {"error", error.what()}}; }
}
Json Tools::build(const Emit& emit, const Cancel& cancel) {
    tests_passed = false;
    int ends[2];
    if (pipe(ends) < 0) throw std::runtime_error("pipe 失败");
    Fd input(ends[0]), output(ends[1]);
    set_fd_flags(input.get());
    set_fd_flags(output.get(), false);
    fs::path log = logs_ / ("build-" + std::to_string(++builds_) + ".log");
    std::ofstream full(log, std::ios::binary);
    if (!full) throw std::runtime_error("创建构建日志失败");
    pid_t child = fork();
    if (child < 0) throw std::runtime_error("fork 构建进程失败");
    if (child == 0) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (chdir(root_.c_str()) < 0 || dup2(output.get(), STDOUT_FILENO) < 0 || dup2(output.get(), STDERR_FILENO) < 0) _exit(126);
        ::close(input.get()); ::close(output.get());
        // No shell command supplied by the model. The copied Makefile is a trusted fixed entry point.
        setenv("LC_ALL", "C", 1);
        unsetenv("MAKEFLAGS"); unsetenv("MFLAGS"); unsetenv("MAKEFILES");
        execlp("make", "make", "--no-print-directory", "test", static_cast<char*>(nullptr));
        _exit(127);
    }
    setpgid(child, child);
    output = Fd();
    auto deadline = Clock::now() + std::chrono::seconds(timeout_);
    std::string feedback, pending, head, tail;
    bool eof = false, exited = false, timeout = false;
    int status = 0;
    std::size_t total = 0;
    auto cleanup = [&] {
        // Also stop descendants that outlive make or keep its output pipe open.
        kill(-child, SIGKILL);
        if (!exited) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    };
    try {
        while (!eof || !exited) {
            if (cancel()) throw Cancelled("构建任务取消或超时");
            if (Clock::now() >= deadline) { timeout = true; break; }
            pollfd fd{input.get(), POLLIN, 0};
            if (poll(&fd, 1, 50) < 0 && errno != EINTR) throw std::runtime_error("构建输出 poll 失败");
            if (fd.revents & (POLLIN | POLLHUP)) {
                char bytes[2048];
                ssize_t size = read(input.get(), bytes, sizeof(bytes));
                if (size > 0) {
                    full.write(bytes, size); full.flush();
                    if (!full) throw std::runtime_error("写构建日志失败");
                    total += static_cast<std::size_t>(size);
                    if (total > 4 * frame_limit) throw std::runtime_error("构建输出超过 4 MiB，已停止");
                    if (feedback.size() < 4096) feedback.append(bytes, std::min<std::size_t>(size, 4096 - feedback.size()));
                    if (head.size() < 1900) head.append(bytes, std::min<std::size_t>(size, 1900 - head.size()));
                    tail.append(bytes, size);
                    if (tail.size() > 1900) tail.erase(0, tail.size() - 1900);
                    pending.append(bytes, size);
                    auto text = utf8_prefix(pending);
                    if (!text.empty()) emit({{"type", "build_output"}, {"text", text}});
                } else if (size == 0) eof = true;
                else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) throw std::runtime_error("读取构建输出失败");
            }
            if (!exited) {
                auto result = waitpid(child, &status, WNOHANG);
                if (result == child) exited = true;
                else if (result < 0 && errno != EINTR) throw std::runtime_error("waitpid 失败");
            }
        }
        cleanup();
        auto tail = utf8_prefix(pending, true);
        if (!tail.empty()) emit({{"type", "build_output"}, {"text", tail}});
    } catch (...) { cleanup(); throw; }
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    tests_passed = !timeout && exit_code == 0;
    if (tests_passed) dirty = false;
    if (total > 4096) feedback = head + "\n... [output truncated; see " + log.filename().string() + "] ...\n" + tail;
    return {{"ok", tests_passed}, {"exit_code", exit_code}, {"timed_out", timeout},
            {"tests_passed", tests_passed}, {"output", feedback}, {"output_truncated", total > feedback.size()},
            {"log_file", log.filename().string()}};
}
} // namespace ca
