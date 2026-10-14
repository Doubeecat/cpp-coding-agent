#include "common.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void help(void) {
    puts("agent-server [--host 127.0.0.1] [--port 8888] [--model-url http://127.0.0.1:8080]\n"
         "  [--project DIR] [--sessions DIR] [--max-rounds 24] [--command-timeout 30]\n"
         "  [--model-timeout 180] [--task-timeout 900] [--input-limit 24576]\n"
         "  [--max-tokens 7680] [--context 32768] [--max-clients 8]");
}
static void reap(pid_t *children, size_t *count, pid_t ended) {
    for (size_t i = 0; i < *count; ++i)
        if (children[i] == ended) {
            children[i] = children[--*count];
            return;
        }
}
int main(int argc, char **argv) {
    Config config;
    config_init(&config);
    CaError error = {0};
    char *project = NULL, *sessions = NULL;
    int listener = -1, code = 1;
    pid_t children[64];
    size_t count = 0;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) {
            help();
            return 0;
        }
        if (++i == argc) {
            ca_error(&error, CA_ERROR, "参数缺少值：%s", key);
            goto done;
        }
        const char *value = argv[i];
        int *number = NULL;
        if (!strcmp(key, "--host"))
            config.host = value;
        else if (!strcmp(key, "--model-url"))
            config.model_url = value;
        else if (!strcmp(key, "--project"))
            config.project = value;
        else if (!strcmp(key, "--sessions"))
            config.sessions = value;
        else if (!strcmp(key, "--port"))
            number = &config.port;
        else if (!strcmp(key, "--max-rounds"))
            number = &config.max_rounds;
        else if (!strcmp(key, "--command-timeout") || !strcmp(key, "--build-timeout"))
            number = &config.command_timeout;
        else if (!strcmp(key, "--model-timeout"))
            number = &config.model_timeout;
        else if (!strcmp(key, "--task-timeout"))
            number = &config.task_timeout;
        else if (!strcmp(key, "--input-limit"))
            number = &config.input_limit;
        else if (!strcmp(key, "--max-tokens"))
            number = &config.output_limit;
        else if (!strcmp(key, "--context"))
            number = &config.context;
        else if (!strcmp(key, "--max-clients"))
            number = &config.max_clients;
        else {
            ca_error(&error, CA_ERROR, "未知参数：%s", key);
            goto done;
        }
        if (number && parse_int_arg(value, number, &error) < 0)
            goto done;
    }
    if (config.port < 0 || config.port > 65535 || config.max_rounds < 1 || config.max_rounds > 64 ||
        config.max_clients < 1 || config.max_clients > 64 || config.context < 1024 ||
        config.context > 65536 || config.input_limit < 1 || config.output_limit < 1 ||
        (int64_t)config.input_limit + config.output_limit + 512 > config.context ||
        config.command_timeout < 1 || config.command_timeout > 300 || config.model_timeout < 1 ||
        config.model_timeout > 600 || config.task_timeout < 1 || config.task_timeout > 1800) {
        ca_error(&error, CA_ERROR, "参数范围非法或上下文预算不足");
        goto done;
    }
    project = realpath(config.project, NULL);
    struct stat st;
    if (!project || stat(project, &st) < 0 || !S_ISDIR(st.st_mode)) {
        ca_error(&error, CA_ERROR, "项目模板必须为目录");
        goto done;
    }
    sessions = path_absolute(config.sessions, &error);
    if (!sessions)
        goto done;
    config.project = project;
    config.sessions = sessions;
    umask(0077);
    if (mkdir_parents(sessions, 0700, &error) < 0)
        goto done;
    listener = listen_tcp(config.host, config.port, &error);
    if (listener < 0)
        goto done;
    install_signals();
    printf("C coding agent: %s:%d | model=%s | sessions=%s\n", config.host,
           socket_port(listener, &error), config.model_url, sessions);
    fflush(stdout);
    while (!stopping) {
        pid_t ended;
        while ((ended = waitpid(-1, NULL, WNOHANG)) > 0)
            reap(children, &count, ended);
        struct pollfd fd = {listener, POLLIN, 0};
        int ready = poll(&fd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            ca_errno(&error, "监听 poll");
            goto done;
        }
        if (!ready)
            continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            ca_errno(&error, "accept");
            goto done;
        }
        if (set_fd_flags(client, true, &error) < 0) {
            close(client);
            goto done;
        }
        if (count >= (size_t)config.max_clients) {
            Json *event = json_event("error");
            json_set_string(event, "message", "服务端连接已满，请稍后重试");
            CaError ignored = {0};
            send_frame(client, JV(event), 1000, &ignored);
            json_free(event);
            close(client);
            continue;
        }
        char id[96];
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        snprintf(id, sizeof(id), "%ld-%lld%09ld", (long)getpid(), (long long)t.tv_sec, t.tv_nsec);
        char *root = path_join(sessions, id, &error);
        if (!root) {
            close(client);
            goto done;
        }
        if (mkdir(root, 0700) < 0) {
            free(root);
            close(client);
            ca_errno(&error, "创建会话目录");
            goto done;
        }
        pid_t child = fork();
        if (child < 0) {
            rmdir(root);
            free(root);
            close(client);
            ca_errno(&error, "fork 会话");
            goto done;
        }
        free(root);
        if (!child) {
            close(listener);
            stopping = 0;
            CaError session_error = {0};
            int result = run_session(client, &config, id, &session_error);
            if (result < 0) {
                fprintf(stderr, "session %s: %s\n", id, session_error.message);
                if (!stopping && !peer_closed(client)) {
                    Json *event = json_event("error");
                    json_set_string(event, "message", session_error.message);
                    CaError ignored = {0};
                    send_frame(client, JV(event), 1000, &ignored);
                    json_free(event);
                }
            }
            close(client);
            free(project);
            free(sessions);
            _exit(result < 0 ? 1 : 0);
        }
        close(client);
        children[count++] = child;
    }
    code = 0;
done:
    close_fd(&listener);
    for (size_t i = 0; i < count; ++i)
        kill(children[i], SIGTERM);
    int64_t deadline = monotonic_ms() + 3000;
    while (count && monotonic_ms() < deadline) {
        pid_t ended = waitpid(-1, NULL, WNOHANG);
        if (ended > 0)
            reap(children, &count, ended);
        else
            sleep_ms(20);
    }
    for (size_t i = 0; i < count; ++i)
        kill(children[i], SIGKILL);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {
    }
    if (code && error.code)
        fprintf(stderr, "server: %s\n", error.message);
    free(project);
    free(sessions);
    return code;
}
