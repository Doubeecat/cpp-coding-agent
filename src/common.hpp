#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ca {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Cancel = std::function<bool()>;
using Emit = std::function<void(const Json&)>;
constexpr std::size_t frame_limit = 1024 * 1024;
extern volatile std::sig_atomic_t stopping;
struct Cancelled : std::runtime_error { using std::runtime_error::runtime_error; };

class Fd {
    int fd_ = -1;
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    int get() const { return fd_; }
    int release();
};
void set_fd_flags(int fd, bool nonblocking = true);
Fd listen_tcp(const std::string& host, int port);
Fd connect_tcp(const std::string& host, int port);
int socket_port(int fd);
void send_frame(int fd, const Json& message, int timeout_ms = 10000);
std::optional<Json> receive_frame(int fd, int timeout_ms = 3600000);
class FrameInbox {
    std::string buffer_;
public:
    bool receive(int fd, const Emit& consume);
};
bool peer_closed(int fd);
void install_signals();
std::string timestamp();
std::string utf8_prefix(std::string& pending, bool final = false);
void append_json(std::ofstream& file, const Json& item);

struct Config {
    std::string host = "127.0.0.1";
    int port = 8888;
    std::string model_url = "http://127.0.0.1:8080";
    std::string model = "qwen3.5-4b";
    fs::path project = fs::path(AGENT_PROJECT_DIR) / "templates/cpp";
    fs::path sessions = fs::path(AGENT_PROJECT_DIR) / "sessions";
    int context = 8192, input_limit = 5632, output_limit = 2048;
    int max_rounds = 8, model_timeout = 180, task_timeout = 480, command_timeout = 30;
    int max_clients = 8;
};

class SseParser {
    std::string buffer_, data_;
    std::function<void(const std::string&)> callback_;
public:
    explicit SseParser(std::function<void(const std::string&)> callback)
        : callback_(std::move(callback)) {}
    void feed(const std::string& bytes);
};
struct ModelReply {
    std::string text, finish_reason;
    int input_tokens = -1, output_tokens = -1;
    Json calls = Json::array();
};
class ModelClient {
    Config config_;
    std::string request(const std::string& path, const std::optional<Json>& body,
                        const Cancel& cancel, const std::function<void(const std::string&)>& stream = {});
public:
    explicit ModelClient(const Config& config);
    ~ModelClient();
    Json get(const std::string& path, const Cancel& cancel);
    Json post(const std::string& path, const Json& body, const Cancel& cancel);
    void check_capacity(const Cancel& cancel);
    int count_tokens(const Json& body, const Cancel& cancel);
    ModelReply complete(const Json& body, const Emit& emit, const Cancel& cancel);
};

Json tool_schema();
class Tools {
    fs::path root_, logs_;
    int timeout_, commands_ = 0;
    fs::path checked_path(const std::string& path) const;
    void write_file(const fs::path& path, const std::string& content) const;
    Json bash(const Json& args, const Emit& emit, const Cancel& cancel);
public:
    Tools(fs::path workspace, fs::path logs, int timeout);
    Json execute(const std::string& name, const Json& args, const Emit& emit, const Cancel& cancel);
};
void copy_project(const fs::path& source, const fs::path& destination);
struct Workspace {
    fs::path root;
    Fd lock;
};
Workspace open_workspace(const fs::path& path);
std::string system_prompt();
void run_session(int socket, const Config& config, const std::string& session_id);
} // namespace ca
