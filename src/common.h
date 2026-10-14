#ifndef CA_COMMON_H
#define CA_COMMON_H
#include "json.h"
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define CA_FRAME_LIMIT ((size_t)1024 * 1024)
#define CA_WORKSPACE_MARKER ".workspace"
typedef enum { CA_ERROR = 1, CA_CANCELLED, CA_TIMEOUT, CA_NOMEM, CA_CONTEXT_LIMIT } CaErrorCode;
typedef struct {
    CaErrorCode code;
    int system_errno;
    char message[1024];
} CaError;
int ca_error(CaError *error, CaErrorCode code, const char *format, ...);
int ca_errno(CaError *error, const char *action);
void ca_clear_error(CaError *error);
typedef struct {
    char *data;
    size_t len, cap;
} Buffer;
void buf_free(Buffer *buffer);
int buf_append(Buffer *buffer, const void *bytes, size_t count, CaError *error);
int buf_printf(Buffer *buffer, CaError *error, const char *format, ...);
void buf_consume(Buffer *buffer, size_t count);
void buf_clear(Buffer *buffer);
const char *buf_text(const Buffer *buffer);
char *ca_strdup(const char *text, CaError *error);
void close_fd(int *fd);
int64_t monotonic_ms(void);
void timestamp(char output[40]);
void sleep_ms(int milliseconds);
size_t utf8_prefix_length(const char *bytes, size_t count, bool final);
int append_json(FILE *file, const JVal *item, CaError *error);
char *path_join(const char *left, const char *right, CaError *error);
char *path_absolute(const char *path, CaError *error);
int mkdir_parents(const char *path, mode_t mode, CaError *error);
int read_file_bytes(const char *path, size_t limit, Buffer *content, CaError *error);
int write_file_bytes(const char *path, const void *content, size_t size, CaError *error);
bool valid_cstring(const JVal *value);
int parse_int_arg(const char *text, int *value, CaError *error);
extern volatile sig_atomic_t stopping;
void install_signals(void);
int set_fd_flags(int fd, bool nonblocking, CaError *error);
int listen_tcp(const char *host, int port, CaError *error);
int connect_tcp(const char *host, int port, CaError *error);
int socket_port(int fd, CaError *error);
int send_frame(int fd, const JVal *message, int timeout_ms, CaError *error);
/* 1: message owned by caller; 0: clean EOF; -1: error. */
int receive_frame(int fd, Json **message, int timeout_ms, CaError *error);
bool peer_closed(int fd);
typedef int (*CaEmit)(void *context, const JVal *event, CaError *error);
typedef bool (*CaCancel)(void *context);
typedef struct {
    CaEmit emit;
    void *emit_ctx;
    CaCancel cancel;
    void *cancel_ctx;
} Callbacks;
int ca_emit(const Callbacks *callbacks, const JVal *event, CaError *error);
bool ca_cancelled(const Callbacks *callbacks);
typedef struct {
    Buffer bytes;
} FrameInbox;
int inbox_receive(FrameInbox *inbox, int fd, CaEmit consume, void *context, CaError *error);

typedef struct {
    const char *host, *model_url, *model, *project, *sessions;
    int port, context, input_limit, output_limit, max_rounds, model_timeout;
    int task_timeout, command_timeout, max_clients;
} Config;
void config_init(Config *config);
typedef struct {
    char *root;
    int lock_fd;
} Workspace;
void workspace_init(Workspace *workspace);
void workspace_destroy(Workspace *workspace);
int open_workspace(const char *path, Workspace *workspace, CaError *error);
int copy_project(const char *source, const char *destination, CaError *error);
typedef struct {
    char *workspace;
    Json *history;
    size_t messages;
} SessionSnapshot;
void snapshot_destroy(SessionSnapshot *snapshot);
int snapshot_load(const char *root, SessionSnapshot *snapshot, CaError *error);
int snapshot_save_workspace(const char *root, const char *workspace, CaError *error);
bool snapshot_has_workspace(const char *root);
Json *list_sessions(const char *root, CaError *error);
typedef struct {
    char *root, *logs;
    int timeout;
    unsigned long commands;
} Tools;
int tools_init(Tools *tools, const char *root, const char *logs, int timeout, CaError *error);
void tools_destroy(Tools *tools);
Json *tool_schema(void);
int tools_execute(Tools *tools, const char *name, const JVal *args, const Callbacks *callbacks,
                  Json **result, CaError *error);

typedef int (*SseConsume)(void *, const char *, size_t, CaError *);
typedef struct {
    Buffer bytes, data;
    SseConsume consume;
    void *context;
} SseParser;
int sse_feed(SseParser *parser, const char *bytes, size_t len, CaError *error);
void sse_destroy(SseParser *parser);
typedef struct {
    const Config *config;
    char *url;
    bool initialized;
} ModelClient;
typedef struct {
    Buffer text;
    char *finish_reason;
    int input_tokens, output_tokens;
    Json *calls;
} ModelReply;
int model_init(ModelClient *model, const Config *config, CaError *error);
void model_destroy(ModelClient *model);
void reply_destroy(ModelReply *reply);
int model_check_capacity(ModelClient *model, const Callbacks *callbacks, CaError *error);
int model_count_tokens(ModelClient *model, const JVal *body, const Callbacks *callbacks, int *count,
                       CaError *error);
int model_complete(ModelClient *model, const JVal *body, const Callbacks *callbacks,
                   ModelReply *reply, CaError *error);
const char *system_prompt(void);
int run_session(int socket, const Config *config, const char *session_id, CaError *error);
#endif
