#include "common.hpp"
#include <curl/curl.h>
#include <regex>

namespace ca {
void SseParser::feed(const std::string& bytes) {
    buffer_ += bytes;
    if (buffer_.size() > frame_limit) throw std::runtime_error("SSE 行过长");
    std::size_t end;
    while ((end = buffer_.find('\n')) != std::string::npos) {
        std::string line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (!data_.empty()) {
                data_.pop_back(); // SSE joins multiple data lines with a newline.
                callback_(data_);
                data_.clear();
            }
        } else if (line.rfind("data:", 0) == 0) {
            std::string value = line.substr(5);
            if (!value.empty() && value.front() == ' ') value.erase(0, 1);
            data_ += value + '\n';
            if (data_.size() > frame_limit) throw std::runtime_error("SSE 事件过长");
        }
    }
}
ModelClient::ModelClient(const Config& config) : config_(config) {
    if (!std::regex_match(config_.model_url, std::regex(R"(^http://(127\.0\.0\.1|localhost|\[::1\])(:[0-9]+)?/?$)")))
        throw std::runtime_error("模型地址只允许本机 HTTP 地址，例如 http://127.0.0.1:8080");
    if (config_.model_url.back() == '/') config_.model_url.pop_back();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("libcurl 初始化失败");
}
ModelClient::~ModelClient() { curl_global_cleanup(); }
std::string ModelClient::request(const std::string& path, const std::optional<Json>& body,
                                const Cancel& cancel, const std::function<void(const std::string&)>& stream) {
    struct State {
        std::string response;
        const Cancel* cancel;
        const std::function<void(const std::string&)>* stream;
        std::exception_ptr failure;
    } state{{}, &cancel, &stream, {}};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init 失败");
    const std::string url = config_.model_url + path;
    const std::string payload = body ? body->dump() : "";
    curl_slist* raw_headers = curl_slist_append(nullptr, "Content-Type: application/json");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(config_.model_timeout));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    if (body) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, +[](char* data, std::size_t size, std::size_t count, void* context) -> std::size_t {
        auto& s = *static_cast<State*>(context);
        try {
            if ((*s.cancel)()) throw Cancelled("任务取消或超时");
            std::size_t bytes = size * count;
            if (s.response.size() + bytes > 8 * frame_limit) throw std::runtime_error("模型响应过大");
            s.response.append(data, bytes);
            if (*s.stream) (*s.stream)(std::string(data, bytes));
            return bytes;
        } catch (...) { s.failure = std::current_exception(); return 0; }
    });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        auto& s = *static_cast<State*>(context);
        try { return (*s.cancel)() ? 1 : 0; }
        catch (...) { s.failure = std::current_exception(); return 1; }
    });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &state);
    auto result = curl_easy_perform(curl.get());
    if (state.failure) std::rethrow_exception(state.failure);
    if (cancel()) throw Cancelled("任务取消或超时");
    if (result != CURLE_OK) throw std::runtime_error(std::string("本地模型请求失败：") + curl_easy_strerror(result));
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) throw std::runtime_error("模型 HTTP " + std::to_string(status) + ": " + state.response.substr(0, 1000));
    return state.response;
}
Json ModelClient::get(const std::string& path, const Cancel& cancel) {
    return Json::parse(request(path, {}, cancel));
}
Json ModelClient::post(const std::string& path, const Json& body, const Cancel& cancel) {
    return Json::parse(request(path, body, cancel));
}
void ModelClient::check_capacity(const Cancel& cancel) {
    auto props = get("/props", cancel);
    auto slots = get("/slots", cancel);
    if (props.value("total_slots", 0) < 2 || !slots.is_array() || slots.size() < 2)
        throw std::runtime_error("模型至少需要两个并行槽，请使用 scripts/start-model.sh 启动");
    for (const auto& slot : slots) {
        if (slot.value("n_ctx", 0) < config_.context)
            throw std::runtime_error("模型单槽上下文不足，请检查 --ctx-size 16384 --parallel 2");
    }
}
int ModelClient::count_tokens(const Json& body, const Cancel& cancel) {
    // This endpoint applies the same template, tool definitions and tokenizer as generation.
    auto result = post("/v1/chat/completions/input_tokens", body, cancel);
    if (!result.contains("input_tokens") || !result["input_tokens"].is_number_integer())
        throw std::runtime_error("模型缺少完整请求 token 计数接口，请更新兼容的 llama.cpp");
    int count = result["input_tokens"].get<int>();
    if (count < 0) throw std::runtime_error("模型返回非法 token 数量");
    return count;
}
ModelReply ModelClient::complete(const Json& body, const Emit& emit, const Cancel& cancel) {
    ModelReply result;
    bool done = false;
    std::map<int, Json> calls;
    SseParser parser([&](const std::string& data) {
        if (data == "[DONE]") { done = true; return; }
        auto event = Json::parse(data);
        if (event.contains("error")) throw std::runtime_error("模型错误：" + event["error"].dump());
        if (event.contains("usage") && event["usage"].is_object()) {
            const auto& usage = event["usage"];
            result.input_tokens = usage.value("prompt_tokens", -1);
            result.output_tokens = usage.value("completion_tokens", -1);
        }
        if (!event.contains("choices") || event["choices"].empty()) return;
        const auto& choice = event["choices"].at(0);
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
            result.finish_reason = choice["finish_reason"].get<std::string>();
        const auto& delta = choice.value("delta", Json::object());
        if (delta.contains("content") && delta["content"].is_string()) {
            std::string text = delta["content"].get<std::string>();
            result.text += text;
            if (!text.empty()) emit({{"type", "text_delta"}, {"text", text}});
        }
        if (delta.contains("tool_calls")) for (const auto& patch : delta["tool_calls"]) {
            int index = patch.at("index").get<int>();
            if (index < 0 || index >= 16) throw std::runtime_error("工具调用数量过多");
            if (!calls.count(index)) calls[index] = {{"id", ""}, {"type", "function"}, {"function", {{"name", ""}, {"arguments", ""}}}};
            auto& call = calls[index];
            if (patch.contains("id") && patch["id"].is_string()) call["id"] = call["id"].get<std::string>() + patch["id"].get<std::string>();
            if (patch.contains("function")) for (const auto* key : {"name", "arguments"}) {
                const auto& function = patch["function"];
                if (function.contains(key) && function[key].is_string()) {
                    std::string value = call["function"][key].get<std::string>() + function[key].get<std::string>();
                    if (value.size() > 128 * 1024) throw std::runtime_error("工具参数过大");
                    call["function"][key] = std::move(value);
                }
            }
        }
    });
    request("/v1/chat/completions", body, cancel, [&](const std::string& bytes) { parser.feed(bytes); });
    if (!done || (result.finish_reason != "stop" && result.finish_reason != "tool_calls"))
        throw std::runtime_error("模型输出被截断或未正常结束，工具未执行");
    for (auto& entry : calls) {
        auto& call = entry.second;
        if (call["id"].get<std::string>().empty() || call["function"]["name"].get<std::string>().empty())
            throw std::runtime_error("模型返回不完整工具调用");
        result.calls.push_back(call);
    }
    return result;
}
} // namespace ca
