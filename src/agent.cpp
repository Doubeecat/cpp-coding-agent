#include "common.hpp"
#include <set>
#include <unistd.h>

namespace ca {
std::string system_prompt() {
    return "You are a software-engineering agent working in a checked-out repository.\n"
           "Use the available tools to inspect files, make focused edits, and run commands.\n"
           "Solve the task, validate when practical, then give a brief final answer.";
}
void run_session(int socket, const Config &config, const std::string &session_id) {
    // Connection state: one active session, one workspace, one history; all of them
    // can be replaced while the connection stays open by /dir, /sessions and /resume.
    std::string session = session_id;
    auto root = config.sessions / session;
    auto workspace = root / "workspace";
    copy_project(config.project, workspace);
    auto selected_workspace = open_workspace(workspace);
    workspace = selected_workspace.root;
    std::ofstream events(root / "events.jsonl", std::ios::app);
    std::ofstream transcript(root / "history.jsonl", std::ios::app);
    if (!events || !transcript)
        throw std::runtime_error("创建会话日志失败");
    Json history = Json::array();
    std::function<void(const Json &)> remember, emit;
    remember = [&](const Json &message) {
        history.push_back(message);
        append_json(transcript, {{"time", timestamp()}, {"message", message}});
    };
    remember({{"role", "system"}, {"content", system_prompt()}});
    std::string request_id;
    emit = [&](const Json &value) {
        Json event = value;
        event["time"] = timestamp();
        event["session_id"] = session;
        event["request_id"] = request_id;
        append_json(events, event);
        send_frame(socket, event);
    };
    SessionSnapshot snapshot;
    snapshot.workspace = workspace;
    // 只在会话第一次建立时记录目录；恢复已有会话时保留原记录，直到目录真的被切换。
    if (!SessionSnapshot::has_workspace(root)) snapshot.save_workspace(root, workspace);
    emit({{"type", "hello"}, {"pid", getpid()}, {"session_id", session}, {"workspace", workspace.string()},
          {"model", config.model}, {"logs", root.string()}, {"context", config.context},
          {"input_limit", config.input_limit}, {"output_limit", config.output_limit}});
    Tools tools(workspace, root, config.command_timeout);
    std::unique_ptr<ModelClient> model;
    bool capacity_checked = false, resumed = false;
    std::set<std::string> seen_requests;
    // Switch the project directory. The session keeps its identity and its logs; only the
    // directory holding tools and the workspace lock changes.
    auto configure_workspace = [&](const Json &request, bool notify) -> Json {
        auto path = request.at("workspace").get<std::string>();
        if (path.empty() || path.find('\0') != std::string::npos || path.size() > 4096)
            throw std::runtime_error("工作目录为空、过长或包含 NUL");
        if (fs::weakly_canonical(fs::absolute(path)) != workspace) {
            auto next = open_workspace(path);
            tools = Tools(next.root, root, config.command_timeout);
            workspace = next.root;
            selected_workspace = std::move(next);
            snapshot.save_workspace(root, workspace);   // 会话记录切换到新的项目目录
        }
        Json ready = {{"type", "workspace_ready"}, {"session_id", session},
                      {"workspace", workspace.string()}, {"model", config.model}};
        if (notify) emit(ready);
        return ready;
    };
    // Replace the current session in place: keep the connection, release the previous
    // workspace lock, load the saved history and re-open the directory that session used.
    auto switch_session = [&](const Json &request) -> Json {
        auto identifier = request.value("session_id", "");
        if (identifier.empty() || identifier.size() > 128 ||
            identifier.find_first_not_of("0123456789-") != std::string::npos)
            throw std::runtime_error("会话 ID 格式不正确");
        if (identifier == session) throw std::runtime_error("已经是当前会话");
        auto target = config.sessions / identifier;
        std::error_code error;
        if (!fs::is_directory(target, error)) throw std::runtime_error("会话不存在：" + identifier);
        SessionSnapshot loaded;
        loaded.load(target);
        std::string warning, restored;
        // 会话日志目录随会话一起切换，命令日志也写进目标会话目录。
        if (events.is_open()) events.close();
        if (transcript.is_open()) transcript.close();
        session = identifier;
        root = target;
        if (!loaded.workspace.empty()) {
            try {
                auto next = open_workspace(loaded.workspace);
                tools = Tools(next.root, root, config.command_timeout);
                workspace = next.root;
                restored = workspace.string();
                selected_workspace = std::move(next);
            } catch (const std::exception &failure) {
                warning = std::string("原工作目录未恢复（") + failure.what() + "），继续使用当前目录";
            }
        }
        history = std::move(loaded.history);
        events.open(root / "events.jsonl", std::ios::app);
        transcript.open(root / "history.jsonl", std::ios::app);
        if (!events || !transcript) throw std::runtime_error("打开会话日志失败");
        request_id.clear();
        model.reset();
        capacity_checked = false;
        resumed = true;
        if (warning.empty() && !restored.empty()) loaded.save_workspace(root, workspace);
        Json ready = {{"type", "session_ready"}, {"session_id", session}, {"workspace", workspace.string()},
                      {"messages", loaded.messages}, {"model", config.model}};
        if (!warning.empty()) ready["warning"] = warning;
        emit(ready);
        return ready;
    };
    while (!stopping) {
        // Outer loop: accept another user task on the same connection.
        auto message = receive_frame(socket);
        if (!message)
            break;
        std::string type = message->value("type", "");
        if (type == "quit")
            break;
        if (type == "list_sessions" || type == "resume_session" || type == "set_workspace") {
            request_id.clear();
            try {
                if (type == "list_sessions") {
                    emit({{"type", "session_list"}, {"sessions", list_sessions(config.sessions)},
                          {"current", session}});
                } else if (type == "resume_session") {
                    switch_session(*message);
                } else {
                    if (!seen_requests.empty() && !resumed)
                        throw std::runtime_error("工作目录只能在首次提问前指定，或先恢复一个会话");
                    configure_workspace(*message, true);
                }
            } catch (const std::exception &error) {
                emit({{"type", type == "resume_session" ? "session_error" : "workspace_error"},
                      {"message", error.what()}, {"session_id", session}});
            }
            continue;
        }
        request_id = message->value("request_id", "");
        if (type != "user_message" || request_id.empty() || request_id.size() > 64 ||
            !message->contains("text") || !(*message)["text"].is_string()) {
            emit({{"type", "error"}, {"message", "需要 user_message、唯一 request_id 和 text 字符串"}});
            emit({{"type", "task_done"}, {"status", "failed"}});
            continue;
        }
        if (!seen_requests.insert(request_id).second || seen_requests.size() > 1024) {
            emit({{"type", "error"}, {"message", "request_id 重复或本连接任务过多"}});
            emit({{"type", "task_done"}, {"status", "failed"}});
            continue;
        }
        auto text = (*message)["text"].get<std::string>();
        if (text.empty() || text.size() > 64 * 1024) {
            emit({{"type", "error"}, {"message", "消息需为 1 到 65536 字节"}});
            emit({{"type", "task_done"}, {"status", "failed"}});
            continue;
        }
        remember({{"role", "user"}, {"content", text}});
        auto deadline = Clock::now() + std::chrono::seconds(config.task_timeout);
        Cancel cancel = [&] { return stopping || peer_closed(socket) || Clock::now() >= deadline; };
        Json pending = Json::array();
        int rounds = 0;
        bool finished = false;
        std::string state = "failed";
        std::string partial_answer;
        Emit stream_emit = [&](const Json &event) {
            if (event.value("type", "") == "text_delta")
                partial_answer += event.value("text", "");
            emit(event);
        };
        try {
            if (!model)
                model = std::make_unique<ModelClient>(config);
            if (!capacity_checked) {
                model->check_capacity(cancel);
                capacity_checked = true;
            }
            // Inner loop: ask the model, execute its tools, then return their results.
            for (; rounds < config.max_rounds; ++rounds) {
                Json body = {{"model", config.model}, {"messages", history}, {"tools", tool_schema()}, {"tool_choice", "auto"}, {"parallel_tool_calls", false}, {"stream", true}, {"stream_options", {{"include_usage", true}}}, {"max_tokens", config.output_limit}, {"temperature", 0.2}, {"top_p", 0.8}, {"presence_penalty", 0.0}, {"repeat_penalty", 1.0}};
                int count = model->count_tokens(body, cancel);
                emit({{"type", "context_usage"}, {"input_tokens", count}, {"context_tokens", count}, {"context", config.context}, {"input_limit", config.input_limit}, {"round", rounds + 1}});
                if (count > config.input_limit || count + config.output_limit + 512 > config.context) {
                    state = "context_limit";
                    throw std::runtime_error("完整对话超过本槽输入预算，已保留历史；请新建连接继续");
                }
                partial_answer.clear();
                auto reply = model->complete(body, stream_emit, cancel);
                Json assistant = {{"role", "assistant"}, {"content", reply.text}};
                pending = reply.calls;
                if (!pending.empty())
                    assistant["tool_calls"] = pending;
                remember(assistant);
                partial_answer.clear();
                int used = reply.input_tokens >= 0 && reply.output_tokens >= 0
                               ? reply.input_tokens + reply.output_tokens
                               : model->count_tokens(
                                     {{"model", config.model}, {"messages", history}, {"tools", tool_schema()}}, cancel);
                emit({{"type", "context_usage"}, {"input_tokens", count}, {"context_tokens", used}, {"context", config.context}, {"input_limit", config.input_limit}, {"round", rounds + 1}});
                if (pending.empty()) {
                    finished = true;
                    state = "success";
                    ++rounds;
                    break;
                }
                while (!pending.empty()) {
                    auto call = pending.at(0);
                    const auto &function = call.at("function");
                    std::string name = function.at("name").get<std::string>();
                    Json result;
                    try {
                        auto args = Json::parse(function.at("arguments").get<std::string>());
                        emit({{"type", "tool_start"}, {"name", name}, {"args", args}, {"tool_call_id", call.at("id")}});
                        Emit tool_emit = [&](const Json &event) {
                            Json detailed = event;
                            detailed["tool_call_id"] = call.at("id");
                            emit(detailed);
                        };
                        result = tools.execute(name, args, tool_emit, cancel);
                    } catch (const Json::exception &error) {
                        result = {{"ok", false}, {"error", std::string("工具参数 JSON 无效：") + error.what()}};
                    }
                    remember({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", result.dump(-1, ' ', false, Json::error_handler_t::replace)}});
                    pending.erase(pending.begin());
                    emit({{"type", "tool_result"}, {"name", name}, {"result", result}, {"tool_call_id", call.at("id")}});
                }
            }
            if (!finished) {
                state = "limit";
                emit({{"type", "error"}, {"message", "已达到模型调用次数上限，请检查日志或提出更小的任务"}});
            }
        } catch (const std::exception &error) {
            if (!partial_answer.empty())
                remember({{"role", "assistant"}, {"content", partial_answer}});
            // Keep tool_call/tool_result pairs complete even when a task is interrupted.
            for (const auto &call : pending)
                remember({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", Json{{"ok", false}, {"error", "工具执行中止"}}.dump()}});
            if (cancel())
                state = Clock::now() >= deadline ? "timeout" : "cancelled";
            if (!peer_closed(socket) && !stopping)
                emit({{"type", "error"}, {"message", error.what()}});
        }
        Json done = {{"type", "task_done"}, {"status", state}, {"rounds", rounds}};
        if (peer_closed(socket) || stopping) {
            done["time"] = timestamp();
            done["session_id"] = session_id;
            done["request_id"] = request_id;
            append_json(events, done);
            break;
        }
        emit(done);
    }
    append_json(events, {{"type", "session_closed"}, {"time", timestamp()}, {"session_id", session_id}});
}
} // namespace ca
