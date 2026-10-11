#include "common.hpp"
#include <set>
#include <unistd.h>

namespace ca {
std::string system_prompt() {
    return R"(You are a small local C++ coding assistant. Answer briefly in Chinese.
The user owns a tiny project in THIS session's working directory. You have exactly four tools.
For every coding task, you can use list_files to learn this project's build contract.
For follow-up edits in the same connection, read only relevant source files; reuse the known build contract.
If src/ is empty, CREATE a small C++17 program in src/main.cpp from the user's requirements.
For existing code, read the relevant files and headers before editing. Preserve working features.
Use read_file with start=1 and count<=100. Paths are relative, e.g. src/main.cpp.
Only src/ is writable. Put .cpp files directly under src/. Build rules and external tests are read-only.
The default template compiles all src/*.cpp into build/app and runs build/app --self-test.
For that template, main(argc, argv) MUST handle --self-test without interactive input:
test real functions with normal and edge cases, print results, return 0 on success and nonzero on failure.
Keep generated programs compact, preferably under 80 lines. Use a shared feature function in normal execution and self-tests.
Use table-driven checks instead of repeating a large block for every test case. Avoid long comments or explanations in tool calls.
Keep self-tests in src/ and maintain them when adding features. Use build/ for temporary test data.
Normal execution should perform the requested feature, with usage instructions in the final answer.
write_file replaces an ENTIRE file with raw source code, not a patch or markdown.
For code generation or edits, actually call write_file. Merely showing code is not completing a coding task.
Always call build_and_test AFTER the last edit. If it fails, use its real error output to fix the source and retry.
Never claim tests passed without a successful build_and_test result after the last edit.
After your edited code passes build_and_test, stop editing and give a brief final answer.
Use small focused changes. Prefer one tool at a time. Keep tool arguments and final answers short.
If the user asks a normal question or to remember something, answer directly; tools are optional.
Earlier messages and tool results belong only to THIS connection. Don't access other sessions.)";
}
void run_session(int socket, const Config &config, const std::string &session_id) {
    // Connection state: one workspace, one history and separate persistent logs.
    auto root = config.sessions / session_id;
    auto workspace = root / "workspace";
    copy_project(config.project, workspace);
    auto selected_workspace = open_workspace(workspace, config);
    workspace = selected_workspace.root;
    std::ofstream events(root / "events.jsonl", std::ios::app);
    std::ofstream transcript(root / "history.jsonl", std::ios::app);
    if (!events || !transcript)
        throw std::runtime_error("创建会话日志失败");
    Json history = Json::array();
    auto remember = [&](const Json &message) {
        history.push_back(message);
        append_json(transcript, {{"time", timestamp()}, {"message", message}});
    };
    remember({{"role", "system"}, {"content", system_prompt()}});
    std::string request_id;
    Emit emit = [&](const Json &value) {
        Json event = value;
        event["time"] = timestamp();
        event["session_id"] = session_id;
        event["request_id"] = request_id;
        append_json(events, event);
        send_frame(socket, event);
    };
    emit({{"type", "hello"}, {"pid", getpid()}, {"workspace", workspace.string()}, {"model", config.model}, {"logs", root.string()}, {"context", config.context}, {"input_limit", config.input_limit}, {"output_limit", config.output_limit}});
    Tools tools(workspace, root, config.build_timeout);
    std::unique_ptr<ModelClient> model;
    bool capacity_checked = false;
    std::set<std::string> seen_requests;
    while (!stopping) {
        // Outer loop: accept another user task on the same connection.
        auto message = receive_frame(socket);
        if (!message)
            break;
        std::string type = message->value("type", "");
        if (type == "quit")
            break;
        if (type == "set_workspace") {
            request_id.clear();
            try {
                if (!seen_requests.empty())
                    throw std::runtime_error("工作目录需在首次提问前指定，请重新连接");
                auto path = message->at("workspace").get<std::string>();
                if (path.empty() || path.find('\0') != std::string::npos || path.size() > 4096)
                    throw std::runtime_error("工作目录为空、过长或包含 NUL");
                if (fs::weakly_canonical(fs::absolute(path)) != workspace) {
                    auto next = open_workspace(path, config);
                    tools = Tools(next.root, root, config.build_timeout);
                    workspace = next.root;
                    selected_workspace = std::move(next);
                }
                emit({{"type", "workspace_ready"}, {"workspace", workspace.string()}, {"model", config.model}});
            } catch (const std::exception &error) {
                emit({{"type", "workspace_error"}, {"message", error.what()}});
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
        bool built = false;
        bool modified = false;
        bool finished = false;
        std::string state = "failed";
        std::string partial_answer;
        Emit stream_emit = [&](const Json &event) {
            if (event.value("type", "") == "text_delta")
                partial_answer += event.value("text", "");
            emit(event);
        };
        auto verified_summary = [&] {
            state = "success";
            emit({{"type", "text_delta"}, {"source", "server"}, {"text", "代码已修改，编译和测试通过。\n"}});
            remember({{"role", "assistant"}, {"content", "代码已修改，编译和测试通过。"}});
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
                Json body = {{"model", config.model}, {"messages", history}, {"tools", tool_schema()}, {"tool_choice", modified && built && tools.tests_passed && !tools.dirty ? "none" : "auto"}, {"parallel_tool_calls", false}, {"stream", true}, {"stream_options", {{"include_usage", true}}}, {"max_tokens", config.output_limit}, {"temperature", 0.2}, {"top_p", 0.8}, {"presence_penalty", 0.0}, {"repeat_penalty", 1.0}};
                int count = model->count_tokens(body, cancel);
                emit({{"type", "context_usage"}, {"input_tokens", count}, {"context_tokens", count}, {"context", config.context}, {"input_limit", config.input_limit}, {"round", rounds + 1}});
                if (count > config.input_limit || count + config.output_limit + 512 > config.context) {
                    if (modified && built && tools.tests_passed && !tools.dirty) {
                        verified_summary();
                        finished = true;
                        break;
                    }
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
                    state = (tools.dirty || (built && !tools.tests_passed)) ? "failed" : "success";
                    if (state == "failed")
                        emit({{"type", "error"}, {"message", "最后的代码尚未通过编译和测试，本轮未标记成功"}});
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
                        if (name == "build_and_test")
                            built = true;
                        Emit tool_emit = [&](const Json &event) {
                            Json detailed = event;
                            detailed["tool_call_id"] = call.at("id");
                            emit(detailed);
                        };
                        result = tools.execute(name, args, tool_emit, cancel);
                        if (name == "write_file" && result.value("ok", false))
                            modified = true;
                    } catch (const Json::exception &error) {
                        result = {{"ok", false}, {"error", std::string("工具参数 JSON 无效：") + error.what()}};
                    }
                    remember({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", result.dump(-1, ' ', false, Json::error_handler_t::replace)}});
                    pending.erase(pending.begin());
                    emit({{"type", "tool_result"}, {"name", name}, {"result", result}, {"tool_call_id", call.at("id")}});
                }
            }
            if (!finished && modified && built && tools.tests_passed && !tools.dirty) {
                verified_summary();
            } else if (!finished) {
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
        Json done = {{"type", "task_done"}, {"status", state}, {"rounds", rounds}, {"tests_passed", tools.tests_passed}, {"unverified_changes", tools.dirty}};
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
