#include "common.hpp"
#include <iostream>
#include <poll.h>
#include <unistd.h>

using namespace ca;
static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Client {
    Fd socket;
    Json hello;
    std::ofstream log;
    std::vector<Json> events;
    int sequence = 0;
    Client(int port, const fs::path& path) : socket(connect_tcp("127.0.0.1", port)), log(path) {
        auto welcome = receive_frame(socket.get(), 10000);
        require(welcome && welcome->value("type", "") == "hello", "缺少 hello");
        hello = *welcome; append_json(log, hello);
    }
    void ask(const std::string& text) {
        events.clear();
        send_frame(socket.get(), {{"type", "user_message"}, {"request_id", std::to_string(++sequence)}, {"text", text}});
    }
    Json receive() {
        auto value = receive_frame(socket.get(), 150000);
        require(value && value->value("request_id", "") == std::to_string(sequence), "连接提前关闭或 request_id 错误");
        append_json(log, *value); events.push_back(*value); return *value;
    }
    std::string answer() const {
        std::string result;
        for (const auto& event : events) if (event.value("type", "") == "text_delta") result += event.value("text", "");
        return result;
    }
};
static bool collect_pair(Client& a, Client& b) {
    bool done[2] = {false, false}; int chunks[2] = {0, 0}; bool overlap = true;
    auto deadline = Clock::now() + std::chrono::seconds(170);
    while (!done[0] || !done[1]) {
        require(Clock::now() < deadline, "并发任务超时");
        pollfd descriptors[2] = {{done[0] ? -1 : a.socket.get(), POLLIN, 0}, {done[1] ? -1 : b.socket.get(), POLLIN, 0}};
        if (poll(descriptors, 2, 200) <= 0) continue;
        for (int i = 0; i < 2; ++i) if (descriptors[i].revents & (POLLIN | POLLHUP)) {
            auto event = (i == 0 ? a : b).receive();
            auto type = event.value("type", "");
            if (type == "text_delta") ++chunks[i];
            if (type == "task_done") {
                overlap = overlap && chunks[0] > 1 && chunks[1] > 1;
                require(event.value("status", "") == "success", "真实模型未成功完成任务：" + event.dump());
                done[i] = true;
            }
        }
    }
    return overlap;
}
int main(int argc, char** argv) {
    fs::path runtime = fs::path(AGENT_PROJECT_DIR) / "runtime";
    fs::create_directories(runtime);
    auto prefix = "live-" + std::to_string(getpid());
    Json report{{"time", timestamp()}, {"backend", "local Qwen3.5-4B-Q8_0"}};
    int code = 0;
    try {
        signal(SIGPIPE, SIG_IGN);
        int port = argc > 1 ? std::stoi(argv[1]) : 8888;
        Client a(port, runtime / (prefix + "-a.jsonl")), b(port, runtime / (prefix + "-b.jsonl"));
        require(a.hello["session_id"] != b.hello["session_id"] && a.hello["pid"] != b.hello["pid"], "会话进程没有隔离");
        report["session_a"] = a.hello["session_id"]; report["session_b"] = b.hello["session_id"];
        report["pid_a"] = a.hello["pid"]; report["pid_b"] = b.hello["pid"];
        a.ask("请记住本连接的项目代号 ALPHA_314159。先写出代号，再用约150字介绍 C++ 的函数。只回答，不操作文件。");
        b.ask("请记住本连接的项目代号 BETA_271828。先写出代号，再用约150字介绍 C++ 的数组。只回答，不操作文件。");
        bool overlap = collect_pair(a, b);
        report["overlapping_streams"] = overlap;
        require(overlap, "两个真实模型响应未在任一完成前同时流式输出");
        std::cout << "PASS 真实模型双客户端同时流式输出" << std::endl;
        a.ask("刚才让你记住的本连接项目代号是什么？只输出代号。");
        b.ask("刚才让你记住的本连接项目代号是什么？只输出代号。");
        collect_pair(a, b);
        report["recall_a"] = a.answer(); report["recall_b"] = b.answer();
        require(a.answer().find("ALPHA_314159") != std::string::npos && a.answer().find("BETA_271828") == std::string::npos, "A 的模型记忆错误");
        require(b.answer().find("BETA_271828") != std::string::npos && b.answer().find("ALPHA_314159") == std::string::npos, "B 的模型记忆错误");
        std::cout << "PASS 同连接连续提问与不同连接上下文隔离" << std::endl;
        a.ask("请用不少于600字介绍 socket、bind、listen、accept 的工作过程。直接回答，不修改文件。");
        b.ask("请用约300字介绍 fork 如何让父子进程同时工作。直接回答，不修改文件。");
        bool started[2] = {false, false};
        auto deadline = Clock::now() + std::chrono::seconds(150);
        while (!started[0] || !started[1]) {
            require(Clock::now() < deadline, "断连测试未开始生成");
            pollfd descriptors[2] = {{started[0] ? -1 : a.socket.get(), POLLIN, 0}, {started[1] ? -1 : b.socket.get(), POLLIN, 0}};
            if (poll(descriptors, 2, 200) <= 0) continue;
            for (int i = 0; i < 2; ++i) if (descriptors[i].revents & (POLLIN | POLLHUP)) {
                auto event = (i == 0 ? a : b).receive();
                require(event.value("type", "") != "task_done", "关闭 A 前任务已经结束");
                if (event.value("type", "") == "text_delta") started[i] = true;
            }
        }
        a.socket = Fd(); report["a_disconnected_at"] = timestamp();
        int later_chunks = 0;
        while (true) {
            auto event = b.receive();
            if (event.value("type", "") == "text_delta") ++later_chunks;
            if (event.value("type", "") == "task_done") {
                require(event.value("status", "") == "success" && later_chunks > 0, "A 断开后 B 未继续输出");
                break;
            }
        }
        report["b_chunks_after_disconnect"] = later_chunks;
        bool reaped = false;
        for (int i = 0; i < 150; ++i) {
            if (kill(a.hello["pid"].get<pid_t>(), 0) < 0) { reaped = true; break; }
            usleep(20000);
        }
        require(reaped, "A 会话进程未回收"); report["disconnected_process_reaped"] = true;
        report["status"] = "passed";
        send_frame(b.socket.get(), {{"type", "quit"}});
        std::cout << "PASS 关闭 A 后 B 继续生成，A 会话已回收" << std::endl;
    } catch (const std::exception& error) {
        report["status"] = "failed"; report["error"] = error.what(); code = 1;
        std::cerr << error.what() << std::endl;
    }
    auto path = runtime / (prefix + "-summary.json");
    std::ofstream(path) << report.dump(2) << '\n';
    std::cout << "验收记录：" << path << std::endl;
    return code;
}
