#include "common.hpp"
#include <cerrno>
#include <cmath>
#include <ctime>
#include <fcntl.h>
#include <fnmatch.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ca {
Json tool_schema() {
    auto property = [](const std::string& type, const std::string& description) {
        return Json{{"type", type}, {"description", description}};
    };
    auto tool = [](const std::string& name, const std::string& description, Json properties, Json required) {
        return Json{{"type", "function"}, {"function", {
            {"name", name}, {"description", description}, {"parameters", {
                {"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}}}}};
    };
    return Json::array({
        tool("Read", "Read a text file. Returns numbered lines.",
             {{"file_path", property("string", "Path to the file to read.")},
              {"offset", property("integer", "Optional 1-based starting line.")},
              {"limit", property("integer", "Optional maximum number of lines.")}}, Json::array({"file_path"})),
        tool("Write", "Write a complete file, creating parent directories when needed.",
             {{"file_path", property("string", "Path to write.")},
              {"content", property("string", "Full file contents.")}}, Json::array({"file_path", "content"})),
        tool("Edit", "Replace an exact string in a text file.",
             {{"file_path", property("string", "Path to edit.")},
              {"old_string", property("string", "Exact text to replace.")},
              {"new_string", property("string", "Replacement text.")},
              {"replace_all", property("boolean", "Replace every occurrence instead of requiring one match.")}},
             Json::array({"file_path", "old_string", "new_string"})),
        tool("Glob", "Find files by glob pattern.",
             {{"pattern", property("string", "Glob pattern, e.g. **/*.py.")},
              {"path", property("string", "Optional directory to search from.")}}, Json::array({"pattern"})),
        tool("Bash", "Run a shell command in the repository workdir.",
             {{"command", property("string", "Command to run.")},
              {"description", property("string", "Short description of the command.")},
              {"timeout", property("number", "Optional timeout in seconds.")}}, Json::array({"command"}))
    });
}
void copy_project(const fs::path& source, const fs::path& destination) {
    if (!fs::is_directory(source)) throw std::runtime_error("项目模板必须为目录");
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
Workspace open_workspace(const fs::path& path) {
    if (path.empty() || path.string().find('\0') != std::string::npos)
        throw std::runtime_error("工作目录不能为空或包含 NUL");
    fs::create_directories(path);
    auto root = fs::canonical(path);
    Fd lock(open((root / ".coding-agent.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.get() < 0) throw std::runtime_error("无法创建工作目录锁");
    if (flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
        throw std::runtime_error("工作目录已被另一个会话使用，请选择其他目录");
    return {root, std::move(lock)};
}
Tools::Tools(fs::path workspace, fs::path logs, int timeout)
    : root_(fs::canonical(workspace)), logs_(std::move(logs)), timeout_(timeout) {}
void SessionSnapshot::load(const fs::path& root) {
    workspace.clear();
    messages = 0;
    history = Json::array();
    std::error_code error;
    if (fs::exists(root / workspace_marker, error)) {
        std::ifstream marker(root / workspace_marker);
        std::string path;
        if (std::getline(marker, path) && !path.empty()) workspace = fs::path(path);
    }
    std::ifstream transcript(root / "history.jsonl");
    if (!transcript) throw std::runtime_error("会话缺少 history.jsonl，无法恢复");
    std::string line;
    while (std::getline(transcript, line)) {
        if (line.empty()) continue;
        Json item;
        try { item = Json::parse(line); }
        catch (const Json::exception&) { continue; }        // 跳过写入过程中留下的残行
        if (!item.contains("message") || !item["message"].is_object()) continue;
        const Json& message = item["message"];
        std::string role = message.value("role", "");
        if (role == "system" || role == "user" || role == "assistant" || role == "tool") {
            history.push_back(message);
            ++messages;
        }
    }
    // 中断的任务可能留下没有结果的工具请求，恢复前补齐，避免发送给模型时报错。
    std::set<std::string> answered;
    for (const auto& message : history)
        if (message.value("role", "") == "tool" && message.contains("tool_call_id"))
            answered.insert(message["tool_call_id"].get<std::string>());
    Json repaired = Json::array();
    for (const auto& message : history) {
        repaired.push_back(message);
        if (message.value("role", "") != "assistant" || !message.contains("tool_calls")) continue;
        for (const auto& call : message["tool_calls"]) {
            std::string id = call.value("id", "");
            if (id.empty() || answered.count(id)) continue;
            repaired.push_back({{"role", "tool"}, {"tool_call_id", id},
                                {"content", Json{{"ok", false}, {"error", "会话中断，工具未执行"}}.dump()}});
            ++messages;
        }
    }
    history = std::move(repaired);
}
void SessionSnapshot::save_workspace(const fs::path& session_root, const fs::path& workspace) const {
    auto target = session_root / workspace_marker;
    auto text = fs::canonical(workspace).string() + "\n";
    auto temporary = target.string() + ".tmp-" + std::to_string(getpid());
    { std::ofstream marker(temporary, std::ios::trunc); if (marker) marker << text; }
    std::error_code error;
    fs::rename(temporary, target, error);
    if (error) fs::remove(temporary, error);
}
bool SessionSnapshot::has_workspace(const fs::path& session_root) {
    std::error_code error;
    return fs::exists(session_root / workspace_marker, error);
}
Json list_sessions(const fs::path& sessions) {
    Json result = Json::array();
    std::error_code error;
    if (!fs::is_directory(sessions, error)) return result;
    for (const auto& entry : fs::directory_iterator(sessions, error)) {
        if (!entry.is_directory()) continue;
        std::string id = entry.path().filename().string();
        if (id.empty() || id[0] == '.') continue;
        SessionSnapshot snapshot;
        try { snapshot.load(entry.path()); }
        catch (const std::exception&) { continue; }
        std::string modified;
        std::error_code stamp_error;
        auto stamp = fs::last_write_time(entry.path() / "history.jsonl", stamp_error);
        if (!stamp_error) {
            // 文件时间的起点与系统时间不同，先算"距今多少秒"再换算成绝对时间。
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                fs::file_time_type::clock::now() - stamp);
            auto seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() - age);
            std::tm value{};
            gmtime_r(&seconds, &value);
            std::ostringstream text;
            text << std::put_time(&value, "%Y-%m-%d %H:%M:%S");
            modified = text.str();
        }
        result.push_back({{"session_id", id},
                          {"workspace", snapshot.workspace.empty() ? Json() : Json(snapshot.workspace.string())},
                          {"messages", snapshot.messages},
                          {"modified", modified.empty() ? Json() : Json(modified)},
                          {"active", fs::exists(entry.path() / "events.jsonl")}});
    }
    std::sort(result.begin(), result.end(), [](const Json& left, const Json& right) {
        return left.value("modified", "") > right.value("modified", "");
    });
    if (result.size() > 200) result.erase(result.begin() + 200, result.end());
    return result;
}
fs::path Tools::checked_path(const std::string& value) const {
    if (value.empty() || value.find('\0') != std::string::npos) throw std::runtime_error("文件路径为空或包含 NUL");
    fs::path path(value);
    path = (path.is_absolute() ? path : root_ / path).lexically_normal();
    auto relative = path.lexically_relative(root_);
    if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("文件路径超出工作目录");
    if (relative == ".coding-agent.lock") throw std::runtime_error("工作目录锁由服务端管理");
    auto checked = root_;
    for (const auto& part : relative) {
        checked /= part;
        if (fs::is_symlink(fs::symlink_status(checked))) throw std::runtime_error("不允许访问符号链接");
    }
    return checked;
}
static std::string read_text_file(const fs::path& path) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > frame_limit)
        throw std::runtime_error("文件不存在、不是普通文件或超过 1 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("打开文件失败");
    std::ostringstream content;
    content << input.rdbuf();
    if (input.bad()) throw std::runtime_error("读取文件失败");
    return content.str();
}
void Tools::write_file(const fs::path& path, const std::string& content) const {
    if (content.size() > 64 * 1024) throw std::runtime_error("文件内容超过 64 KiB");
    if (fs::exists(path) && !fs::is_regular_file(path)) throw std::runtime_error("目标不是普通文件");
    fs::create_directories(path.parent_path());
    auto temp = path.string() + ".tmp-" + std::to_string(getpid());
    Fd file(open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (file.get() < 0) throw std::runtime_error("创建临时文件失败");
    try {
        if (fs::exists(path)) {
            auto mode = static_cast<mode_t>(fs::status(path).permissions()) & 0777;
            if (fchmod(file.get(), mode) < 0) throw std::runtime_error("保留文件权限失败");
        }
        std::size_t offset = 0;
        while (offset < content.size()) {
            auto bytes = write(file.get(), content.data() + offset, content.size() - offset);
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes <= 0) throw std::runtime_error("写文件失败");
            offset += static_cast<std::size_t>(bytes);
        }
        if (fsync(file.get()) < 0) throw std::runtime_error("保存文件失败");
        fs::rename(temp, path);
    } catch (...) { fs::remove(temp); throw; }
}
static std::vector<std::string> path_parts(const fs::path& path) {
    std::vector<std::string> parts;
    for (const auto& part : path) if (part != ".") parts.push_back(part.string());
    return parts;
}
// lexically_relative(".") keeps a leading "./", so relative paths are rebuilt
// from their components to keep one stable spelling for matching and output.
static std::string relative_name(const fs::path& path) {
    auto parts = path_parts(path);
    std::string name;
    for (const auto& part : parts) name += name.empty() ? part : "/" + part;
    return name;
}
static bool glob_matches(const std::vector<std::string>& pattern, const fs::path& path) {
    auto parts = path_parts(path);
    std::vector<bool> previous(parts.size() + 1, false);
    previous[0] = true;
    for (const auto& part : pattern) {
        std::vector<bool> next(parts.size() + 1, false);
        if (part == "**") next[0] = previous[0];
        for (std::size_t i = 1; i <= parts.size(); ++i)
            next[i] = part == "**" ? previous[i] || next[i - 1]
                                  : previous[i - 1] && fnmatch(part.c_str(), parts[i - 1].c_str(), 0) == 0;
        previous = std::move(next);
    }
    return previous.back();
}
Json Tools::execute(const std::string& name, const Json& args, const Emit& emit, const Cancel& cancel) {
    try {
        if (cancel()) throw Cancelled("任务取消或超时");
        if (!args.is_object()) throw std::runtime_error("工具参数必须为 JSON 对象");
        // Validate against the schema advertised to the model, including optional types.
        auto schemas = tool_schema();
        auto schema = std::find_if(schemas.begin(), schemas.end(), [&](const Json& tool) {
            return tool["function"]["name"] == name;
        });
        if (schema == schemas.end()) throw std::runtime_error("未知工具：" + name);
        const auto& parameters = (*schema)["function"]["parameters"];
        for (const auto& key : parameters["required"])
            if (!args.contains(key.get<std::string>())) throw std::runtime_error("缺少参数：" + key.get<std::string>());
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (!parameters["properties"].contains(it.key())) throw std::runtime_error("未知参数：" + it.key());
            auto type = parameters["properties"][it.key()]["type"].get<std::string>();
            bool valid = type == "string" ? it->is_string() : type == "integer" ? it->is_number_integer()
                       : type == "boolean" ? it->is_boolean() : it->is_number();
            if (!valid) throw std::runtime_error("参数类型无效：" + it.key());
        }
        if (name == "Glob") {
            auto pattern = args.at("pattern").get<std::string>();
            if (pattern.empty() || pattern.size() > 4096 || pattern.find('\0') != std::string::npos)
                throw std::runtime_error("glob pattern 为空、过长或包含 NUL");
            fs::path glob(pattern);
            if (glob.is_absolute()) throw std::runtime_error("glob pattern 需相对于搜索目录");
            for (const auto& part : glob) if (part == "..") throw std::runtime_error("glob pattern 不允许 ..");
            auto base = checked_path(args.value("path", "."));
            if (!fs::is_directory(base)) throw std::runtime_error("搜索路径不是目录");
            auto parts = path_parts(glob);
            Json files = Json::array();
            bool truncated = false;
            std::size_t bytes = 0;
            for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (cancel()) throw Cancelled("文件搜索取消或超时");
                auto name = it->path().filename();
                if (name == ".git" || name == ".coding-agent.lock") {
                    if (it->is_directory()) it.disable_recursion_pending();
                    continue;
                }
                if (it->is_symlink()) continue;
                if (it->is_regular_file() && glob_matches(parts, it->path().lexically_relative(base))) {
                    auto relative = relative_name(it->path().lexically_relative(root_));
                    bytes += Json(relative).dump().size() + 1;
                    if (files.size() == 100 || bytes > 4096) { truncated = true; break; }
                    files.push_back(relative);
                }
            }
            std::sort(files.begin(), files.end());
            return {{"ok", true}, {"files", files}, {"truncated", truncated}};
        }
        if (name == "Read") {
            auto path = args.at("file_path").get<std::string>();
            auto positive_line = [&](const char* key, int fallback) {
                if (!args.contains(key)) return fallback;
                if (args[key] < 1 || args[key] > std::numeric_limits<int>::max())
                    throw std::runtime_error(std::string(key) + " 必须为正整数且不超过 INT_MAX");
                return args[key].get<int>();
            };
            int start = positive_line("offset", 1), count = positive_line("limit", 200);
            std::istringstream input(read_text_file(checked_path(path)));
            std::string line, text;
            int number = 0, last = start - 1;
            bool truncated = false, line_truncated = false;
            while (std::getline(input, line)) {
                if (++number < start) continue;
                if (static_cast<long long>(number) >= static_cast<long long>(start) + count) { truncated = true; break; }
                std::string entry = std::to_string(number) + ": " + line + '\n';
                if (text.size() + entry.size() > 4096) {
                    truncated = true;
                    if (text.empty()) {
                        entry.resize(4096);
                        text = utf8_prefix(entry);
                        last = number;
                        line_truncated = true;
                    }
                    break;
                }
                text += entry;
                last = number;
            }
            return {{"ok", true}, {"file_path", path}, {"content", text}, {"truncated", truncated},
                    {"line_truncated", line_truncated}, {"next_offset", last + 1}};
        }
        if (name == "Write") {
            auto path = checked_path(args.at("file_path").get<std::string>());
            auto content = args.at("content").get<std::string>();
            write_file(path, content);
            return {{"ok", true}, {"file_path", args["file_path"]}, {"bytes", content.size()}};
        }
        if (name == "Edit") {
            auto path = checked_path(args.at("file_path").get<std::string>());
            auto content = read_text_file(path);
            auto old_string = args.at("old_string").get<std::string>();
            auto new_string = args.at("new_string").get<std::string>();
            if (old_string.empty()) throw std::runtime_error("old_string 不能为空");
            bool all = args.value("replace_all", false);
            std::size_t matches = 0, position = 0;
            while ((position = content.find(old_string, position)) != std::string::npos) {
                ++matches;
                position += old_string.size();
            }
            if (!matches) throw std::runtime_error("old_string 未匹配任何内容");
            if (!all && matches != 1) throw std::runtime_error("old_string 匹配多处；请提供唯一内容或设置 replace_all=true");
            std::string updated;
            std::size_t begin = 0;
            while ((position = content.find(old_string, begin)) != std::string::npos) {
                updated.append(content, begin, position - begin);
                updated += new_string;
                if (updated.size() > 64 * 1024) throw std::runtime_error("编辑后的文件超过 64 KiB");
                begin = position + old_string.size();
            }
            updated.append(content, begin, std::string::npos);
            write_file(path, updated);
            return {{"ok", true}, {"file_path", args["file_path"]}, {"replacements", matches}, {"bytes", updated.size()}};
        }
        return bash(args, emit, cancel);
    } catch (const Cancelled&) { throw; }
      catch (const std::exception& error) { return {{"ok", false}, {"error", error.what()}}; }
}
Json Tools::bash(const Json& args, const Emit& emit, const Cancel& cancel) {
    auto command = args.at("command").get<std::string>();
    if (command.empty() || command.find('\0') != std::string::npos) throw std::runtime_error("command 为空或包含 NUL");
    double seconds = args.value("timeout", static_cast<double>(timeout_));
    auto now = Clock::now();
    if (!std::isfinite(seconds) || seconds <= 0 ||
        seconds >= std::chrono::duration<double>(Clock::time_point::max() - now).count())
        throw std::runtime_error("timeout 必须为有限的正秒数");
    auto deadline = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    int ends[2];
    if (pipe(ends) < 0) throw std::runtime_error("pipe 失败");
    Fd input(ends[0]), output(ends[1]);
    set_fd_flags(input.get());
    set_fd_flags(output.get(), false);
    fs::path log = logs_ / ("command-" + std::to_string(++commands_) + ".log");
    std::ofstream full(log, std::ios::binary);
    if (!full) throw std::runtime_error("创建命令日志失败");
    Fd null(open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (null.get() < 0) throw std::runtime_error("打开 /dev/null 失败");
    pid_t child = fork();
    if (child < 0) throw std::runtime_error("fork 命令进程失败");
    if (child == 0) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (chdir(root_.c_str()) < 0 || dup2(null.get(), STDIN_FILENO) < 0 ||
            dup2(output.get(), STDOUT_FILENO) < 0 || dup2(output.get(), STDERR_FILENO) < 0) _exit(126);
        ::close(input.get()); ::close(output.get());
        setenv("LC_ALL", "C", 1);
        execl("/bin/bash", "bash", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    setpgid(child, child);
    output = Fd();
    std::string feedback, pending, head, tail;
    bool eof = false, exited = false, timeout = false;
    int status = 0;
    std::size_t total = 0;
    auto cleanup = [&] {
        // Also stop descendants that outlive the shell or keep its output pipe open.
        kill(-child, SIGKILL);
        if (!exited) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    };
    try {
        while (!eof || !exited) {
            if (cancel()) throw Cancelled("命令任务取消或超时");
            if (Clock::now() >= deadline) { timeout = true; break; }
            pollfd fd{input.get(), POLLIN, 0};
            if (poll(&fd, 1, 50) < 0 && errno != EINTR) throw std::runtime_error("命令输出 poll 失败");
            if (fd.revents & (POLLIN | POLLHUP)) {
                char bytes[2048];
                ssize_t size = read(input.get(), bytes, sizeof(bytes));
                if (size > 0) {
                    full.write(bytes, size); full.flush();
                    if (!full) throw std::runtime_error("写命令日志失败");
                    total += static_cast<std::size_t>(size);
                    if (total > 4 * frame_limit) throw std::runtime_error("命令输出超过 4 MiB，已停止");
                    if (feedback.size() < 4096) feedback.append(bytes, std::min<std::size_t>(size, 4096 - feedback.size()));
                    if (head.size() < 1900) head.append(bytes, std::min<std::size_t>(size, 1900 - head.size()));
                    tail.append(bytes, size);
                    if (tail.size() > 1900) tail.erase(0, tail.size() - 1900);
                    pending.append(bytes, size);
                    auto text = utf8_prefix(pending);
                    if (!text.empty()) emit({{"type", "tool_output"}, {"text", text}});
                } else if (size == 0) eof = true;
                else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) throw std::runtime_error("读取命令输出失败");
            }
            if (!exited) {
                auto result = waitpid(child, &status, WNOHANG);
                if (result == child) exited = true;
                else if (result < 0 && errno != EINTR) throw std::runtime_error("waitpid 失败");
            }
        }
        cleanup();
        auto tail = utf8_prefix(pending, true);
        if (!tail.empty()) emit({{"type", "tool_output"}, {"text", tail}});
    } catch (...) { cleanup(); throw; }
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    bool ok = !timeout && exit_code == 0;
    if (total > 4096) feedback = head + "\n... [output truncated; see " + log.filename().string() + "] ...\n" + tail;
    return {{"ok", ok}, {"exit_code", exit_code}, {"timed_out", timeout},
            {"output", feedback}, {"output_truncated", total > 4096},
            {"log_file", log.filename().string()}};
}
} // namespace ca
