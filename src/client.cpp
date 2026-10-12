#include "client_tui.hpp"
#include <cstdlib>
#include <iostream>
#include <unistd.h>

int main(int argc, char **argv) {
    using namespace ca;
    std::string host = "127.0.0.1";
    int port = 8888;
    bool json_output = false, plain = false, verbose = false;
    fs::path workdir;
    std::vector<std::string> prompts;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") {
                std::cout << "agent-client [--host 127.0.0.1] [--port 8888] [--workdir DIR]\n"
                             "             [--plain | --verbose | --json] [--prompt TEXT ...]\n"
                             "交互终端默认使用 TUI；--workdir 直接读写指定目录。\n"
                             "--plain 简略文本；--verbose 完整工具内容；--json 完整 JSON 事件。\n";
                return 0;
            }
            if (key == "--json") {
                json_output = true;
                continue;
            }
            if (key == "--plain" || key == "--verbose") {
                plain = true;
                verbose = key == "--verbose" || verbose;
                continue;
            }
            if (i + 1 == argc)
                throw std::runtime_error("参数缺少值：" + key);
            std::string value = argv[++i];
            if (key == "--host")
                host = value;
            else if (key == "--port")
                port = std::stoi(value);
            else if (key == "--prompt")
                prompts.push_back(value);
            else if (key == "--workdir") {
                if (value.empty()) throw std::runtime_error("工作目录不能为空");
                workdir = fs::absolute(value).lexically_normal();
            }
            else
                throw std::runtime_error("未知参数：" + key);
        }
        if (port < 1 || port > 65535)
            throw std::runtime_error("端口范围非法");
        install_signals();
        auto socket = connect_tcp(host, port);
        std::string pending;
        UiState state;
        auto display = [&](const Json &message) {
            if (json_output) {
                std::cout << message.dump() << std::endl;
                return;
            }
            auto type = message.value("type", "");
            state.apply(message);
            if (type == "hello")
                std::cout << "已连接，会话 " << message.value("session_id", "")
                          << "\n代码目录：" << state.workspace << "\n模型：" << state.model
                          << " · 上下文上限：" << state.context << " tokens\n输入 /quit 退出\n"
                          << std::flush;
            else if (type == "text_delta" || (type == "tool_output" && verbose)) {
                pending += message.value("text", "");
                std::cout << utf8_prefix(pending) << std::flush;
            } else if (type == "tool_start") {
                std::cout << '\n' << state.summary(static_cast<int>(state.entries.size()) - 1) << '\n';
                if (verbose) std::cout << message.value("args", Json::object()).dump(2) << '\n';
                std::cout << std::flush;
            }
            else if (type == "tool_result") {
                std::cout << state.summary(static_cast<int>(state.entries.size()) - 1) << '\n';
                if (verbose) std::cout << message.at("result").dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
                std::cout << std::flush;
            } else if (type == "error" || type == "workspace_error")
                std::cout << "\n[错误] " << message.value("message", "") << '\n'
                          << std::flush;
            else if (type == "task_done")
                std::cout << utf8_prefix(pending, true) << "\n[本轮 " << message.value("status", "failed") << "]\n"
                          << std::flush;
        };
        auto welcome = receive_frame(socket.get());
        if (!welcome)
            throw std::runtime_error("服务端未返回欢迎消息");
        if (welcome->value("type", "") != "hello") {
            display(*welcome);
            return 1;
        }
        Json hello = *welcome;
        if (json_output) display(hello);
        if (!workdir.empty()) {
            send_frame(socket.get(), {{"type", "set_workspace"}, {"workspace", workdir.string()}});
            auto ready = receive_frame(socket.get());
            if (!ready) throw std::runtime_error("服务端在设置工作目录时断开");
            if (ready->value("type", "") != "workspace_ready") {
                display(*ready);
                return 1;
            }
            if (json_output) display(*ready);
            hello["workspace"] = ready->at("workspace");
            hello["model"] = ready->at("model");
        }
        const char* term = std::getenv("TERM");
        if (!json_output && !plain && prompts.empty() && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
            term && *term && std::string(term) != "dumb")
            return run_tui(socket.get(), hello);
        if (!json_output) display(hello);
        int counter = 0, exit_code = 0;
        auto ask = [&](const std::string &text) {
            std::string id = std::to_string(++counter);
            state.ask(text, id);
            send_frame(socket.get(), {{"type", "user_message"}, {"request_id", id}, {"text", text}});
            for (;;) {
                auto message = receive_frame(socket.get());
                if (!message)
                    throw std::runtime_error("服务端在任务结束前断开");
                if (message->value("request_id", "") != id)
                    throw std::runtime_error("服务端返回错误的 request_id");
                display(*message);
                if (message->value("type", "") == "task_done") {
                    if (message->value("status", "failed") != "success")
                        exit_code = 1;
                    break;
                }
            }
        };
        if (!prompts.empty())
            for (const auto &prompt : prompts)
                ask(prompt);
        else {
            std::string line;
            while (!stopping) {
                if (!json_output && isatty(STDIN_FILENO))
                    std::cout << "\n你> " << std::flush;
                if (!std::getline(std::cin, line) || line == "/quit")
                    break;
                if (!line.empty())
                    ask(line);
            }
        }
        send_frame(socket.get(), {{"type", "quit"}});
        return exit_code;
    } catch (const std::exception &error) {
        std::cerr << "client: " << error.what() << '\n';
        return 1;
    }
}
