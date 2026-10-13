#include "common.hpp"
#include <cerrno>
#include <iostream>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void help() {
    std::cout << "agent-server [--host 127.0.0.1] [--port 8888] [--model-url http://127.0.0.1:8080]\n"
              << "  [--project DIR] [--sessions DIR] [--max-rounds 24] [--command-timeout 30]\n"
              << "  [--model-timeout 180] [--task-timeout 900] [--input-limit 24576]\n"
              << "  [--max-tokens 7680] [--context 32768] [--max-clients 8]\n";
}
int main(int argc, char** argv) {
    using namespace ca;
    Config config;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") { help(); return 0; }
            if (i + 1 == argc) throw std::runtime_error("参数缺少值：" + key);
            std::string value = argv[++i];
            if (key == "--host") config.host = value;
            else if (key == "--port") config.port = std::stoi(value);
            else if (key == "--model-url") config.model_url = value;
            else if (key == "--project") config.project = value;
            else if (key == "--sessions") config.sessions = value;
            else if (key == "--max-rounds") config.max_rounds = std::stoi(value);
            else if (key == "--command-timeout" || key == "--build-timeout") config.command_timeout = std::stoi(value);
            else if (key == "--model-timeout") config.model_timeout = std::stoi(value);
            else if (key == "--task-timeout") config.task_timeout = std::stoi(value);
            else if (key == "--input-limit") config.input_limit = std::stoi(value);
            else if (key == "--max-tokens") config.output_limit = std::stoi(value);
            else if (key == "--context") config.context = std::stoi(value);
            else if (key == "--max-clients") config.max_clients = std::stoi(value);
            else throw std::runtime_error("未知参数：" + key);
        }
        if (config.port < 0 || config.port > 65535 || config.max_rounds < 1 || config.max_rounds > 64 ||
            config.max_clients < 1 || config.max_clients > 64 || config.context < 1024 || config.context > 65536 ||
            config.input_limit < 1 || config.output_limit < 1 || config.input_limit + config.output_limit + 512 > config.context ||
            config.command_timeout < 1 || config.command_timeout > 300 || config.model_timeout < 1 || config.model_timeout > 600 ||
            config.task_timeout < 1 || config.task_timeout > 1800)
            throw std::runtime_error("参数范围非法或上下文预算不足");
        config.project = fs::canonical(config.project);
        if (!fs::is_directory(config.project)) throw std::runtime_error("项目模板必须为目录");
        config.sessions = fs::absolute(config.sessions).lexically_normal();
        umask(0077);
        fs::create_directories(config.sessions);
        auto listener = listen_tcp(config.host, config.port);
        install_signals();
        std::set<pid_t> children;
        std::cout << "C++ coding agent: " << config.host << ':' << socket_port(listener.get())
                  << " | model=" << config.model_url << " | sessions=" << config.sessions << std::endl;
        while (!stopping) {
            int status;
            pid_t ended;
            while ((ended = waitpid(-1, &status, WNOHANG)) > 0) children.erase(ended);
            pollfd fd{listener.get(), POLLIN, 0};
            int ready = poll(&fd, 1, 200);
            if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("监听 poll 失败"); }
            if (!ready) continue;
            Fd client(accept(listener.get(), nullptr, nullptr));
            if (client.get() < 0) { if (errno == EINTR || errno == EAGAIN) continue; throw std::runtime_error("accept 失败"); }
            set_fd_flags(client.get());
            if (children.size() >= static_cast<std::size_t>(config.max_clients)) {
                try { send_frame(client.get(), {{"type", "error"}, {"message", "服务端连接已满，请稍后重试"}}); }
                catch (const std::exception&) {}
                continue;
            }
            auto id = std::to_string(getpid()) + "-" + std::to_string(Clock::now().time_since_epoch().count());
            if (!fs::create_directory(config.sessions / id)) throw std::runtime_error("会话目录冲突");
            pid_t child = fork();
            if (child < 0) { fs::remove(config.sessions / id); throw std::runtime_error("fork 会话失败"); }
            if (child == 0) {
                listener = Fd();
                children.clear();
                stopping = 0;
                int code = 0;
                try { run_session(client.get(), config, id); }
                catch (const std::exception& error) {
                    code = 1;
                    std::cerr << "session " << id << ": " << error.what() << std::endl;
                    if (!stopping && !peer_closed(client.get())) try {
                        send_frame(client.get(), {{"type", "error"}, {"message", error.what()}});
                    } catch (...) {}
                }
                client = Fd();
                _exit(code);
            }
            children.insert(child);
        }
        listener = Fd();
        for (pid_t child : children) kill(child, SIGTERM);
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!children.empty() && Clock::now() < deadline) {
            int status; pid_t child = waitpid(-1, &status, WNOHANG);
            if (child > 0) children.erase(child);
            else usleep(20000);
        }
        for (pid_t child : children) kill(child, SIGKILL);
        while (waitpid(-1, nullptr, 0) > 0 || errno == EINTR) {}
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "server: " << error.what() << '\n';
        return 1;
    }
}
