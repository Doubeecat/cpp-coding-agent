#pragma once
#include "common.hpp"

namespace ca {
struct UiEntry {
    enum Kind { User, Assistant, Tool, Notice } kind;
    std::string text, name, output;
    Json args = Json::object(), result;
    bool expanded = false, running = false;
    UiEntry(Kind type, std::string content = {}) : kind(type), text(std::move(content)) {}
};
class UiState {
    int active_tool_ = -1;
    int round_ = 0;
    Clock::time_point started_{};
    double completed_seconds_ = 0;
public:
    std::vector<UiEntry> entries;
    std::string workspace, model, session, request_id, status = "就绪";
    int context = 8192, tokens = -1, input_limit = 5632, exit_code = 0;
    bool busy = false;
    void apply(const Json& event);
    void ask(const std::string& text, const std::string& id);
    void toggle(int index);
    std::string summary(int index) const;
    std::string details(int index) const;
    double elapsed_seconds() const;
};
int run_tui(int socket, const Json& hello);
} // namespace ca
