#include "common.h"
#include <curl/curl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void sse_destroy(SseParser *p) {
    buf_free(&p->bytes);
    buf_free(&p->data);
    p->consume = NULL;
    p->context = NULL;
}
int sse_feed(SseParser *p, const char *bytes, size_t n, CaError *error) {
    if (n > CA_FRAME_LIMIT - p->bytes.len)
        return ca_error(error, CA_ERROR, "SSE 行过长");
    if (buf_append(&p->bytes, bytes, n, error) < 0)
        return -1;
    for (;;) {
        char *newline = memchr(buf_text(&p->bytes), '\n', p->bytes.len);
        if (!newline)
            break;
        size_t length = (size_t)(newline - p->bytes.data), consumed = length + 1;
        if (length && p->bytes.data[length - 1] == '\r')
            --length;
        if (!length) {
            if (p->data.len) {
                --p->data.len;
                p->data.data[p->data.len] = 0;
                if (p->consume &&
                    p->consume(p->context, buf_text(&p->data), p->data.len, error) < 0)
                    return -1;
                buf_clear(&p->data);
            }
        } else if (length >= 5 && !memcmp(p->bytes.data, "data:", 5)) {
            size_t start = 5;
            if (start < length && p->bytes.data[start] == ' ')
                ++start;
            if (length - start + 1 > CA_FRAME_LIMIT - p->data.len)
                return ca_error(error, CA_ERROR, "SSE 事件过长");
            if (buf_append(&p->data, p->bytes.data + start, length - start, error) < 0 ||
                buf_append(&p->data, "\n", 1, error) < 0)
                return -1;
        }
        buf_consume(&p->bytes, consumed);
    }
    return 0;
}
int model_init(ModelClient *model, const Config *config, CaError *error) {
    memset(model, 0, sizeof(*model));
    const char *url = config->model_url;
    if (strncmp(url, "http://", 7))
        return ca_error(error, CA_ERROR, "模型地址只允许本机 HTTP 地址");
    const char *host = url + 7, *tail = NULL;
    if (!strncmp(host, "127.0.0.1", 9))
        tail = host + 9;
    else if (!strncmp(host, "localhost", 9))
        tail = host + 9;
    else if (!strncmp(host, "[::1]", 5))
        tail = host + 5;
    if (!tail)
        return ca_error(error, CA_ERROR, "模型地址只允许本机 HTTP 地址");
    if (*tail == ':') {
        ++tail;
        const char *first = tail;
        unsigned port = 0;
        while (*tail >= '0' && *tail <= '9') {
            port = port * 10 + (unsigned)(*tail++ - '0');
            if (port > 65535)
                break;
        }
        if (tail == first || port > 65535)
            return ca_error(error, CA_ERROR, "模型地址端口无效");
    }
    if (*tail && strcmp(tail, "/"))
        return ca_error(error, CA_ERROR, "模型地址只允许本机 HTTP 地址和根路径");
    model->url = ca_strdup(url, error);
    if (!model->url)
        return -1;
    size_t length = strlen(model->url);
    if (length && model->url[length - 1] == '/')
        model->url[length - 1] = 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        free(model->url);
        model->url = NULL;
        return ca_error(error, CA_ERROR, "libcurl 初始化失败");
    }
    model->initialized = true;
    model->config = config;
    return 0;
}
void model_destroy(ModelClient *model) {
    if (model->initialized)
        curl_global_cleanup();
    free(model->url);
    memset(model, 0, sizeof(*model));
}
void reply_destroy(ModelReply *reply) {
    buf_free(&reply->text);
    free(reply->finish_reason);
    json_free(reply->calls);
    memset(reply, 0, sizeof(*reply));
}
typedef struct {
    Buffer response;
    const Callbacks *callbacks;
    SseParser *stream;
    CaError failure;
} HttpState;
static size_t http_write(char *data, size_t size, size_t count, void *context) {
    HttpState *state = context;
    if (ca_cancelled(state->callbacks)) {
        ca_error(&state->failure, CA_CANCELLED, "任务取消或超时");
        return 0;
    }
    if (size && count > SIZE_MAX / size) {
        ca_error(&state->failure, CA_ERROR, "模型响应长度溢出");
        return 0;
    }
    size_t n = size * count;
    if (n > 8 * CA_FRAME_LIMIT - state->response.len) {
        ca_error(&state->failure, CA_ERROR, "模型响应过大");
        return 0;
    }
    if (buf_append(&state->response, data, n, &state->failure) < 0 ||
        (state->stream && sse_feed(state->stream, data, n, &state->failure) < 0))
        return 0;
    return n;
}
static int http_progress(void *context, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                         curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return ca_cancelled(((HttpState *)context)->callbacks) ? 1 : 0;
}
static int request(ModelClient *model, const char *path, const JVal *body, const Callbacks *cb,
                   SseParser *stream, Buffer *response, CaError *error) {
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    char *payload = NULL;
    Buffer url = {0};
    HttpState state = {.callbacks = cb, .stream = stream};
    int code = -1;
    if (!curl)
        return ca_error(error, CA_NOMEM, "curl_easy_init 失败");
    if (buf_printf(&url, error, "%s%s", model->url, path) < 0)
        goto done;
    size_t payload_size = 0;
    if (body) {
        payload = json_dump_value(body, false, &payload_size);
        if (!payload) {
            ca_error(error, CA_NOMEM, "JSON 序列化失败");
            goto done;
        }
    }
    headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (!headers) {
        ca_error(error, CA_NOMEM, "内存不足");
        goto done;
    }
#define OPT(option, value)                                                                         \
    do {                                                                                           \
        if (curl_easy_setopt(curl, option, value) != CURLE_OK) {                                   \
            ca_error(error, CA_ERROR, "设置 HTTP 选项失败");                                       \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
    OPT(CURLOPT_URL, url.data);
    OPT(CURLOPT_HTTPHEADER, headers);
    OPT(CURLOPT_CONNECTTIMEOUT, 5L);
    OPT(CURLOPT_TIMEOUT, (long)model->config->model_timeout);
    OPT(CURLOPT_NOSIGNAL, 1L);
    OPT(CURLOPT_PROXY, "");
    OPT(CURLOPT_NOPROXY, "*");
    OPT(CURLOPT_FOLLOWLOCATION, 0L);
    if (body) {
        OPT(CURLOPT_POSTFIELDS, payload);
        OPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)payload_size);
    }
    OPT(CURLOPT_WRITEFUNCTION, http_write);
    OPT(CURLOPT_WRITEDATA, &state);
    OPT(CURLOPT_NOPROGRESS, 0L);
    OPT(CURLOPT_XFERINFOFUNCTION, http_progress);
    OPT(CURLOPT_XFERINFODATA, &state);
#undef OPT
    CURLcode result = curl_easy_perform(curl);
    if (state.failure.code) {
        *error = state.failure;
        goto done;
    }
    if (ca_cancelled(cb)) {
        ca_error(error, CA_CANCELLED, "任务取消或超时");
        goto done;
    }
    if (result != CURLE_OK) {
        ca_error(error, CA_ERROR, "本地模型请求失败：%s", curl_easy_strerror(result));
        goto done;
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        ca_error(error, CA_ERROR, "模型 HTTP %ld: %.800s", status, buf_text(&state.response));
        goto done;
    }
    *response = state.response;
    memset(&state.response, 0, sizeof(state.response));
    code = 0;
done:
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(payload);
    buf_free(&url);
    buf_free(&state.response);
    return code;
}
static int request_json(ModelClient *model, const char *path, const JVal *body, const Callbacks *cb,
                        Json **result, CaError *error) {
    Buffer response = {0};
    int code = request(model, path, body, cb, NULL, &response, error);
    if (!code) {
        *result = json_parse(buf_text(&response), response.len);
        if (!*result)
            code = ca_error(error, CA_ERROR, "模型返回无效 JSON");
    }
    buf_free(&response);
    return code;
}
int model_check_capacity(ModelClient *model, const Callbacks *cb, CaError *error) {
    Json *props = NULL, *slots = NULL;
    int code = -1;
    if (request_json(model, "/props", NULL, cb, &props, error) < 0 ||
        request_json(model, "/slots", NULL, cb, &slots, error) < 0)
        goto done;
    if (json_int(JV(props), "total_slots", 0) < 2 || !json_is_array(JV(slots)) ||
        json_len(JV(slots)) < 2) {
        ca_error(error, CA_ERROR, "模型至少需要两个并行槽，请使用 scripts/start-model.sh 启动");
        goto done;
    }
    for (size_t i = 0; i < json_len(JV(slots)); ++i)
        if (json_int(json_at(JV(slots), i), "n_ctx", 0) < model->config->context) {
            ca_error(error, CA_ERROR, "模型单槽上下文不足，请检查 --ctx-size 65536 --parallel 2");
            goto done;
        }
    code = 0;
done:
    json_free(props);
    json_free(slots);
    return code;
}
int model_count_tokens(ModelClient *model, const JVal *body, const Callbacks *cb, int *count,
                       CaError *error) {
    Json *result = NULL;
    if (request_json(model, "/v1/chat/completions/input_tokens", body, cb, &result, error) < 0)
        return -1;
    int64_t value = json_int(JV(result), "input_tokens", -1);
    int code = 0;
    if (value < 0 || value > INT_MAX)
        code = ca_error(error, CA_ERROR, "模型缺少完整请求 token 计数接口或返回非法 token 数量");
    else
        *count = (int)value;
    json_free(result);
    return code;
}
typedef struct {
    Buffer id, name, arguments;
    bool used;
} ToolPatch;
typedef struct {
    ModelReply *reply;
    const Callbacks *callbacks;
    bool done;
    ToolPatch calls[16];
} Completion;
static int completion_event(void *context, const char *bytes, size_t len, CaError *error) {
    Completion *state = context;
    if (len == 6 && !memcmp(bytes, "[DONE]", 6)) {
        state->done = true;
        return 0;
    }
    Json *event = json_parse(bytes, len);
    if (!event || !json_is_object(JV(event))) {
        json_free(event);
        return ca_error(error, CA_ERROR, "SSE 返回无效 JSON");
    }
    int code = -1;
    if (json_get(JV(event), "error")) {
        char *text = json_dump_value(json_get(JV(event), "error"), false, NULL);
        ca_error(error, CA_ERROR, "模型错误：%.800s", text ? text : "unknown");
        free(text);
        goto done;
    }
    JVal *usage = json_get(JV(event), "usage");
    if (json_is_object(usage)) {
        int64_t input = json_int(usage, "prompt_tokens", -1),
                output = json_int(usage, "completion_tokens", -1);
        if (input > INT_MAX || output > INT_MAX) {
            ca_error(error, CA_ERROR, "模型 usage 数值越界");
            goto done;
        }
        state->reply->input_tokens = input >= 0 ? (int)input : -1;
        state->reply->output_tokens = output >= 0 ? (int)output : -1;
    }
    JVal *choices = json_get(JV(event), "choices");
    if (!json_len(choices)) {
        code = 0;
        goto done;
    }
    if (!json_is_array(choices)) {
        ca_error(error, CA_ERROR, "模型 choices 必须为数组");
        goto done;
    }
    JVal *choice = json_at(choices, 0), *reason = json_get(choice, "finish_reason");
    if (json_is_string(reason)) {
        char *next = ca_strdup(json_str(reason), error);
        if (!next)
            goto done;
        free(state->reply->finish_reason);
        state->reply->finish_reason = next;
    }
    JVal *delta = json_get(choice, "delta"), *content = json_get(delta, "content");
    if (json_is_string(content)) {
        if (buf_append(&state->reply->text, json_str(content), json_len(content), error) < 0)
            goto done;
        if (json_len(content)) {
            Json *text = json_event("text_delta");
            json_set_value(text, "text", content);
            if (!text || text->failed) {
                json_free(text);
                ca_error(error, CA_NOMEM, "内存不足");
                goto done;
            }
            int emitted = ca_emit(state->callbacks, JV(text), error);
            json_free(text);
            if (emitted < 0)
                goto done;
        }
    }
    JVal *patches = json_get(delta, "tool_calls");
    if (patches && !json_is_array(patches)) {
        ca_error(error, CA_ERROR, "模型 tool_calls 必须为数组");
        goto done;
    }
    for (size_t i = 0; i < json_len(patches); ++i) {
        JVal *patch = json_at(patches, i);
        int64_t index = json_int(patch, "index", -1);
        if (index < 0 || index >= 16) {
            ca_error(error, CA_ERROR, "工具调用数量过多或 index 无效");
            goto done;
        }
        ToolPatch *call = &state->calls[index];
        call->used = true;
        JVal *id = json_get(patch, "id");
        if (json_is_string(id) && buf_append(&call->id, json_str(id), json_len(id), error) < 0)
            goto done;
        JVal *f = json_get(patch, "function");
        const char *keys[] = {"name", "arguments"};
        Buffer *buffers[] = {&call->name, &call->arguments};
        for (size_t k = 0; k < 2; ++k) {
            JVal *value = json_get(f, keys[k]);
            if (!json_is_string(value))
                continue;
            if (json_len(value) > 128 * 1024 - buffers[k]->len) {
                ca_error(error, CA_ERROR, "工具参数过大");
                goto done;
            }
            if (buf_append(buffers[k], json_str(value), json_len(value), error) < 0)
                goto done;
        }
    }
    code = 0;
done:
    json_free(event);
    return code;
}
int model_complete(ModelClient *model, const JVal *body, const Callbacks *cb, ModelReply *reply,
                   CaError *error) {
    memset(reply, 0, sizeof(*reply));
    reply->input_tokens = reply->output_tokens = -1;
    reply->calls = json_array();
    if (!reply->calls)
        return ca_error(error, CA_NOMEM, "内存不足");
    Completion completion = {.reply = reply, .callbacks = cb};
    SseParser parser = {.consume = completion_event, .context = &completion};
    Buffer response = {0};
    int code = -1;
    if (request(model, "/v1/chat/completions", body, cb, &parser, &response, error) < 0)
        goto done;
    if (!completion.done || !reply->finish_reason ||
        (strcmp(reply->finish_reason, "stop") && strcmp(reply->finish_reason, "tool_calls"))) {
        ca_error(error, CA_ERROR, "模型输出被截断或未正常结束，工具未执行");
        goto done;
    }
    for (size_t i = 0; i < 16; ++i) {
        ToolPatch *p = &completion.calls[i];
        if (!p->used)
            continue;
        if (!p->id.len || !p->name.len || memchr(p->id.data, 0, p->id.len) ||
            memchr(p->name.data, 0, p->name.len)) {
            ca_error(error, CA_ERROR, "模型返回不完整工具调用");
            goto done;
        }
        Json *call = json_object();
        json_set_stringn(call, "id", p->id.data, p->id.len);
        json_set_string(call, "type", "function");
        JVal *function = json_new_object(call);
        json_add_string(call, function, "name", p->name.data, p->name.len);
        json_add_string(call, function, "arguments", buf_text(&p->arguments), p->arguments.len);
        json_add(call, JV(call), "function", function);
        if (!call || call->failed || json_append(reply->calls, JV(call)) < 0) {
            json_free(call);
            ca_error(error, CA_NOMEM, "内存不足");
            goto done;
        }
        json_free(call);
    }
    code = 0;
done:
    for (size_t i = 0; i < 16; ++i) {
        buf_free(&completion.calls[i].id);
        buf_free(&completion.calls[i].name);
        buf_free(&completion.calls[i].arguments);
    }
    buf_free(&response);
    sse_destroy(&parser);
    if (code < 0)
        reply_destroy(reply);
    return code;
}
