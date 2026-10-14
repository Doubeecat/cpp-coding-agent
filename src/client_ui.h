#ifndef CA_CLIENT_UI_H
#define CA_CLIENT_UI_H
#include "common.h"

typedef enum { UI_USER, UI_ASSISTANT, UI_TOOL, UI_NOTICE } UiKind;
typedef struct {
    UiKind kind;
    Buffer text, name, output;
    Json *args, *result;
    bool expanded, running;
} UiEntry;
typedef struct {
    UiEntry *entries;
    size_t count, capacity;
    Buffer workspace, model, session, request_id, status, history, history_warning;
    int context, tokens, input_limit, exit_code, active_tool, round;
    bool busy, restoring;
    int64_t started;
    double completed_seconds;
} UiState;
int ui_init(UiState *state, CaError *error);
void ui_destroy(UiState *state);
int ui_add(UiState *state, UiKind kind, const char *text, size_t len, CaError *error);
int ui_apply(UiState *state, const JVal *event, CaError *error);
int ui_ask(UiState *state, const char *text, size_t len, const char *id, CaError *error);
void ui_toggle(UiState *state, int index);
int ui_summary(const UiState *state, int index, Buffer *text, CaError *error);
int ui_details(const UiState *state, int index, Buffer *text, CaError *error);
double ui_elapsed(const UiState *state);
/* Takes ownership of initial and leaves it empty. */
int run_tui(int socket, UiState *initial, CaError *error);
#endif
