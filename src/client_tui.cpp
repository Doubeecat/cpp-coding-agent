#define _XOPEN_SOURCE_EXTENDED 1
#include "client_tui.hpp"
#include <curses.h>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <poll.h>
#include <sstream>
#include <iomanip>
#include <sys/socket.h>
#include <unistd.h>

namespace ca {
void UiState::ask(const std::string &text, const std::string &id) {
    request_id = id;
    busy = true;
    status = "思考中";
    started_ = Clock::now();
    completed_seconds_ = 0;
    round_ = 0;
    active_tool_ = -1;
    entries.push_back({UiEntry::User, text});
}
double UiState::elapsed_seconds() const {
    return busy ? std::chrono::duration<double>(Clock::now() - started_).count() : completed_seconds_;
}
void UiState::apply(const Json &event) {
    auto type = event.value("type", "");
    if (type == "hello" || type == "workspace_ready") {
        workspace = event.value("workspace", workspace);
        model = event.value("model", model);
        session = event.value("session_id", session);
        context = event.value("context", context);
        input_limit = event.value("input_limit", input_limit);
    } else if (type == "context_usage") {
        tokens = event.value("context_tokens", event.value("input_tokens", tokens));
        context = event.value("context", context);
        input_limit = event.value("input_limit", input_limit);
        int round = event.value("round", round_);
        if (round != round_) { round_ = round; status = "思考中"; }
    } else if (type == "text_delta") {
        status = "生成中";
        if (entries.empty() || entries.back().kind != UiEntry::Assistant)
            entries.push_back({UiEntry::Assistant, ""});
        entries.back().text += event.value("text", "");
    } else if (type == "tool_start") {
        UiEntry entry{UiEntry::Tool, ""};
        entry.name = event.value("name", "");
        entry.args = event.value("args", Json::object());
        entry.running = true;
        status = entry.name == "build_and_test" ? "编译测试中" : "工具执行中";
        entries.push_back(std::move(entry));
        active_tool_ = static_cast<int>(entries.size()) - 1;
    } else if (type == "build_output" && active_tool_ >= 0) {
        auto &output = entries[active_tool_].output;
        constexpr std::size_t limit = 128 * 1024;
        if (output.size() < limit) {
            auto text = event.value("text", "");
            bool truncated = text.size() >= limit - output.size();
            text.resize(std::min(text.size(), limit - output.size()));
            output += utf8_prefix(text);
            if (truncated)
                output += "\n[界面内容已截断，完整输出保存在构建日志]\n";
        }
    } else if (type == "tool_result") {
        // Invalid JSON arguments can fail before the server emits tool_start.
        if (active_tool_ < 0) {
            UiEntry entry{UiEntry::Tool};
            entry.name = event.value("name", "工具");
            entries.push_back(std::move(entry));
            active_tool_ = static_cast<int>(entries.size()) - 1;
        }
        auto &entry = entries[active_tool_];
        entry.result = event.at("result");
        entry.running = false;
        active_tool_ = -1;
        status = "思考中";
    } else if (type == "error" || type == "workspace_error") {
        entries.push_back({UiEntry::Notice, event.value("message", "未知错误")});
    } else if (type == "task_done") {
        if (busy) completed_seconds_ = elapsed_seconds();
        busy = false;
        auto outcome = event.value("status", "failed");
        status = outcome == "success" ? "完成" : outcome == "failed" ? "失败" : outcome == "cancelled" ? "已取消" :
                 outcome == "timeout" ? "已超时" : outcome == "context_limit" ? "上下文已满" : outcome == "limit" ? "达到轮次上限" : outcome;
        if (outcome != "success")
            exit_code = 1;
        if (active_tool_ >= 0)
            entries[active_tool_].running = false;
        active_tool_ = -1;
    }
}
void UiState::toggle(int index) {
    if (index >= 0 && index < static_cast<int>(entries.size()) && entries[index].kind == UiEntry::Tool)
        entries[index].expanded = !entries[index].expanded;
}
std::string UiState::summary(int index) const {
    const auto &entry = entries.at(index);
    std::string text = (entry.expanded ? "▾ " : "▸ ") + entry.name;
    if (entry.args.contains("path") && entry.args["path"].is_string())
        text += "  " + entry.args["path"].get<std::string>();
    if (entry.running)
        return text + "  · 执行中";
    if (entry.result.is_null())
        return text + "  · 已中止";
    const auto &result = entry.result;
    if (!result.value("ok", false))
        return text + "  · 失败";
    text += "  · 完成";
    if (result.contains("files"))
        text += "  " + std::to_string(result["files"].size()) + " 个文件";
    if (result.contains("bytes"))
        text += "  " + std::to_string(result["bytes"].get<std::size_t>()) + " B";
    if (entry.name == "read_file")
        text += "  " + std::to_string(result.value("content", "").size()) + " B";
    if (entry.name == "build_and_test")
        text += "  exit=" + std::to_string(result.value("exit_code", -1));
    return text;
}
std::string UiState::details(int index) const {
    const auto &entry = entries.at(index);
    std::string text = "参数 JSON\n" + entry.args.dump(2, ' ', false, Json::error_handler_t::replace);
    if (entry.args.contains("content") && entry.args["content"].is_string())
        text += "\n源码\n" + entry.args["content"].get<std::string>();
    if (!entry.output.empty())
        text += "\n构建输出\n" + entry.output;
    if (!entry.result.is_null())
        text += "\n结果 JSON\n" + entry.result.dump(2, ' ', false, Json::error_handler_t::replace);
    return text;
}
static std::wstring wide(const std::string &text) {
    std::wstring result;
    std::mbstate_t state{};
    for (std::size_t i = 0; i < text.size();) {
        wchar_t ch;
        auto size = std::mbrtowc(&ch, text.data() + i, text.size() - i, &state);
        if (size == static_cast<std::size_t>(-2))
            break; // An unfinished streamed character stays hidden.
        if (size == static_cast<std::size_t>(-1)) {
            result += L'�';
            state = {};
            ++i;
        } else {
            result += ch;
            i += size ? size : 1;
        }
    }
    return result;
}
static std::string narrow(const std::wstring &text) {
    std::string result;
    std::mbstate_t state{};
    for (wchar_t ch : text) {
        char bytes[MB_LEN_MAX];
        auto size = std::wcrtomb(bytes, ch, &state);
        if (size == static_cast<std::size_t>(-1))
            throw std::runtime_error("终端字符编码失败");
        result.append(bytes, size);
    }
    return result;
}
static int cells(const std::wstring &text) {
    int count = 0;
    for (wchar_t ch : text)
        count += std::max(0, wcwidth(ch));
    return count;
}
static std::wstring clip(const std::wstring &text, int width) {
    std::wstring result;
    int used = 0;
    for (wchar_t ch : text) {
        int size = std::max(0, wcwidth(ch));
        if (used + size > width)
            break;
        result += ch;
        used += size;
    }
    return result;
}
static std::wstring path_tail(const std::wstring &text, int width) {
    if (cells(text) <= width)
        return text;
    if (width <= 1)
        return L"…";
    std::wstring tail;
    int used = 1;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        int size = std::max(0, wcwidth(*it));
        if (used + size > width)
            break;
        tail.insert(tail.begin(), *it);
        used += size;
    }
    return L"…" + tail;
}
struct Row {
    std::wstring text;
    int entry = -1, color = 0;
    bool header = false;
};
static void wrap_rows(std::vector<Row> &rows, const std::string &text, int width, int entry, int color) {
    std::wstring line;
    int used = 0;
    for (wchar_t ch : wide(text)) {
        if (ch == L'\r')
            continue;
        if (ch == L'\n') {
            rows.push_back({line, entry, color});
            line.clear();
            used = 0;
            continue;
        }
        if (ch == L'\t')
            ch = L' ';
        if (ch < 32 || ch == 127)
            ch = L'?';
        int size = std::max(0, wcwidth(ch));
        if (used + size > width && !line.empty()) {
            rows.push_back({line, entry, color});
            line.clear();
            used = 0;
        }
        line += ch;
        used += size;
    }
    rows.push_back({line, entry, color});
}
struct EditorLine {
    std::wstring text;
    std::size_t begin, end;
};
static std::vector<EditorLine> editor_lines(const std::wstring& input, int width) {
    std::vector<EditorLine> lines;
    std::wstring text;
    std::size_t begin = 0;
    int used = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        wchar_t ch = input[i];
        if (ch == L'\n') { lines.push_back({text, begin, i}); text.clear(); begin = i + 1; used = 0; continue; }
        if (ch == L'\t') ch = L' ';
        int size = std::max(0, wcwidth(ch));
        if (used + size > width && !text.empty()) { lines.push_back({text, begin, i}); text.clear(); begin = i; used = 0; }
        text += ch; used += size;
    }
    lines.push_back({text, begin, input.size()});
    return lines;
}
static std::size_t editor_position(const EditorLine& line, int column) {
    int used = 0;
    for (std::size_t i = 0; i < line.text.size(); ++i) {
        int size = std::max(0, wcwidth(line.text[i]));
        if (used + size > column) return line.begin + i;
        used += size;
    }
    return line.end;
}
enum { MouseLegacy = KEY_MAX + 1, MouseSgr, PasteBegin, PasteEnd, Newline };
struct InputEvent {
    int kind = ERR;
    wint_t key = 0;
    int x = 0, y = 0, scroll = 0;
    bool click = false;
};
class Terminal {
    int mouse_encoding_ = 0;
    std::string mouse_bytes_;
    Clock::time_point mouse_started_;
    InputEvent mouse(unsigned button, unsigned x, unsigned y, bool released = false) {
        InputEvent event;
        if (!x || !y || x > 100000 || y > 100000 || released) return event;
        event.kind = KEY_CODE_YES; event.key = KEY_MOUSE;
        event.x = static_cast<int>(x) - 1; event.y = static_cast<int>(y) - 1;
        if (button & 64) event.scroll = (button & 3) == 0 ? -3 : (button & 3) == 1 ? 3 : 0;
        else event.click = (button & 35) == 0;
        return event;
    }
public:
    Terminal() {
        if (!std::setlocale(LC_ALL, "") || MB_CUR_MAX == 1)
            if (!std::setlocale(LC_ALL, "C.UTF-8"))
                std::setlocale(LC_ALL, "en_US.UTF-8");
        if (!initscr())
            throw std::runtime_error("终端初始化失败");
        raw();
        noecho();
        keypad(stdscr, true);
        timeout(50);
        set_escdelay(25);
        if (has_colors()) {
            start_color();
            use_default_colors();
            init_pair(1, COLOR_CYAN, -1);
            init_pair(2, COLOR_YELLOW, -1);
            init_pair(3, COLOR_RED, -1);
        }
        // Read both encodings directly: Apple's ncurses mouse ABI cannot report wheel-down.
        define_key("\x1b[M", MouseLegacy);
        define_key("\x1b[<", MouseSgr);
        define_key("\x1b[200~", PasteBegin);
        define_key("\x1b[201~", PasteEnd);
        define_key("\x1b\r", Newline);
        define_key("\x1b\n", Newline);
        std::fputs("\x1b[?1000h\x1b[?1006h\x1b[?2004h", stdout);
        std::fflush(stdout);
    }
    ~Terminal() {
        std::fputs("\x1b[?1000l\x1b[?1006l\x1b[?2004l", stdout);
        std::fflush(stdout);
        curs_set(1);
        endwin();
    }
    InputEvent read() {
        InputEvent event;
        if (!mouse_encoding_) {
            event.kind = get_wch(&event.key);
            if (event.kind != KEY_CODE_YES || (event.key != MouseLegacy && event.key != MouseSgr)) return event;
            mouse_encoding_ = static_cast<int>(event.key); mouse_bytes_.clear(); mouse_started_ = Clock::now();
            event = {};
        }
        // Partial reports never wait for the remaining bytes; the next UI iteration resumes them.
        timeout(0);
        while (mouse_encoding_) {
            int ch = getch();
            if (ch == ERR) break;
            if (ch > 255) { mouse_encoding_ = 0; break; }
            mouse_bytes_ += static_cast<char>(ch);
            if (mouse_encoding_ == MouseLegacy && mouse_bytes_.size() == 3) {
                unsigned button = static_cast<unsigned char>(mouse_bytes_[0]);
                unsigned x = static_cast<unsigned char>(mouse_bytes_[1]), y = static_cast<unsigned char>(mouse_bytes_[2]);
                if (button >= 32 && x >= 33 && y >= 33) event = mouse(button - 32, x - 32, y - 32);
                mouse_encoding_ = 0;
            } else if (mouse_encoding_ == MouseSgr && (ch == 'M' || ch == 'm')) {
                unsigned button, x, y; char end; int consumed = 0;
                if (std::sscanf(mouse_bytes_.c_str(), "%u;%u;%u%c%n", &button, &x, &y, &end, &consumed) == 4 &&
                    consumed == static_cast<int>(mouse_bytes_.size())) event = mouse(button, x, y, end == 'm');
                mouse_encoding_ = 0;
            } else if (mouse_encoding_ == MouseSgr && (mouse_bytes_.size() >= 64 || (ch != ';' && (ch < '0' || ch > '9')))) {
                mouse_encoding_ = 0;
            }
        }
        if (Clock::now() - mouse_started_ > std::chrono::milliseconds(500)) mouse_encoding_ = 0;
        timeout(50);
        return event;
    }
};
int run_tui(int socket, const Json &hello) {
    Terminal terminal;
    UiState state;
    state.apply(hello);
    FrameInbox inbox;
    std::wstring input;
    std::size_t cursor = 0;
    int top = 0, selected = -1, sequence = 0, editor_top = 0, last_width = 0, last_height = 0;
    bool following = true, input_focus = true, connected = true, ensure_selected = false;
    bool ensure_cursor = true, pasting = false, paste_cr = false;
    std::string hint;
    while (!stopping) {
        pollfd net{socket, POLLIN, 0};
        if (connected && poll(&net, 1, 0) > 0) {
            connected = inbox.receive(socket, [&](const Json &event) {
                if (event.value("request_id", "") != state.request_id)
                    throw std::runtime_error("服务端返回错误的 request_id");
                state.apply(event);
            });
            if (!connected) {
                state.apply({{"type", "task_done"}, {"status", "disconnected"}});
                state.status = "连接已关闭";
            }
        }
        int height, width;
        getmaxyx(stdscr, height, width);
        erase();
        if (height < 8 || width < 45) {
            mvaddstr(0, 0, "Terminal too small (45x8). Ctrl+C to exit.");
            refresh();
            auto small_key = terminal.read();
            if (small_key.kind != ERR && (small_key.key == 3 || small_key.key == 4))
                break;
            continue;
        }
        auto put = [&](int row, int col, const std::wstring &text, int color = 0, bool reverse = false) {
            int attr = color ? COLOR_PAIR(color) : A_NORMAL;
            if (reverse)
                attr |= A_REVERSE;
            attrset(attr);
            auto limited = clip(text, width - col - 1);
            mvaddnwstr(row, col, limited.c_str(), static_cast<int>(limited.size()));
            attrset(A_NORMAL);
        };
        auto editor = editor_lines(input, width - 8);
        int cursor_line = 0;
        for (int i = 0; i < static_cast<int>(editor.size()); ++i)
            if (editor[i].begin <= cursor) cursor_line = i;
        int cursor_column = cells(editor[cursor_line].text.substr(0, cursor - editor[cursor_line].begin));
        int input_height = std::min(static_cast<int>(editor.size()), std::max(1, height - 8));
        int input_row = height - 2 - input_height, separator = input_row - 1;
        if (width != last_width || height != last_height) ensure_cursor = true;
        last_width = width; last_height = height;
        if (ensure_cursor) {
            if (cursor_line < editor_top) editor_top = cursor_line;
            if (cursor_line >= editor_top + input_height) editor_top = cursor_line - input_height + 1;
            ensure_cursor = false;
        }
        editor_top = std::clamp(editor_top, 0, static_cast<int>(editor.size()) - input_height);
        std::wstring heading = L"Coding Agent  · ";
        if (state.busy) heading += std::wstring(1, L"|/-\\"[static_cast<int>(state.elapsed_seconds() * 8) % 4]) + L" ";
        heading += wide(state.status);
        if (!state.request_id.empty()) {
            std::ostringstream elapsed; elapsed << std::fixed << std::setprecision(1) << state.elapsed_seconds();
            heading += L" · " + wide(elapsed.str()) + L"s";
        }
        put(0, 1, heading, 1);
        std::vector<Row> rows;
        for (int i = 0; i < static_cast<int>(state.entries.size()); ++i) {
            const auto &entry = state.entries[i];
            if (entry.kind == UiEntry::Tool) {
                int color = entry.running ? 2 : entry.result.is_object() && entry.result.value("ok", false) ? 1
                                                                                                            : 3;
                rows.push_back({wide(state.summary(i)), i, color, true});
                if (entry.expanded)
                    wrap_rows(rows, state.details(i), width - 4, i, 0);
            } else {
                auto label = entry.kind == UiEntry::User ? L"你" : entry.kind == UiEntry::Assistant ? L"助手"
                                                                                                    : L"提示";
                rows.push_back({label, i, entry.kind == UiEntry::Notice ? 3 : 1});
                wrap_rows(rows, entry.text, width - 4, i, 0);
            }
            rows.push_back({L""});
        }
        if (rows.empty())
            wrap_rows(rows, "输入需求，开始生成或修改 C++ 程序。\n工具默认折叠：点击标题，或 Tab 选中后按 Enter 展开。", width - 4, -1, 0);
        int body = separator - 1, maximum = std::max(0, static_cast<int>(rows.size()) - body);
        if (following)
            top = maximum;
        top = std::clamp(top, 0, maximum);
        if (ensure_selected) {
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (rows[i].header && rows[i].entry == selected) {
                    if (i < top)
                        top = i;
                    if (i >= top + body)
                        top = i - body + 1;
                    break;
                }
            top = std::clamp(top, 0, maximum);
            ensure_selected = false;
        }
        for (int i = 0; i < body && top + i < static_cast<int>(rows.size()); ++i) {
            const auto &row = rows[top + i];
            put(i + 1, row.header ? 1 : 2, row.text, row.color, row.header && !input_focus && row.entry == selected);
        }
        if (maximum > 0) {
            auto position = wide(std::to_string(top + 1) + "–" + std::to_string(std::min(top + body, static_cast<int>(rows.size()))) +
                                 "/" + std::to_string(rows.size()) + " 行");
            if (cells(heading) + cells(position) + 4 < width) put(0, width - cells(position) - 2, position, 2);
            mvvline(1, width - 1, ACS_VLINE, body);
            int thumb = std::max(1, body * body / static_cast<int>(rows.size()));
            int start = top * (body - thumb) / maximum;
            mvvline(1 + start, width - 1, ACS_CKBOARD, thumb);
        }
        mvhline(separator, 0, ACS_HLINE, width);
        if (static_cast<int>(editor.size()) > input_height)
            put(separator, 2, wide("输入 " + std::to_string(editor_top + 1) + "–" + std::to_string(editor_top + input_height) +
                                  "/" + std::to_string(editor.size()) + " 行"), 2);
        put(input_row, 1, L"你 > ", 1);
        for (int i = 0; i < input_height; ++i) put(input_row + i, 6, editor[editor_top + i].text);
        auto help = hint.empty() ? (input_focus ? "Enter 发送 · Alt+Enter 换行 · Tab 工具 · 滚轮滚动 · Ctrl+C 退出" : "Enter 展开 · ↑↓ 工具 · Esc 输入 · 滚轮/PgUp/PgDn 滚动 · Ctrl+C 退出") : hint;
        put(height - 2, 1, wide(help), 2);
        auto token_text = wide(state.tokens < 0 ? "—" : std::to_string(state.tokens)) + L"/" + wide(std::to_string(state.context));
        auto model = wide(state.model);
        int model_width = std::max(1, std::min(32, width - 8 - cells(L" | Token " + token_text + L" | 模型 ")));
        if (cells(model) > model_width)
            model = clip(model, model_width - 1) + L"…";
        auto tail = L" | Token " + token_text + L" | 模型 " + model;
        auto footer = L"目录 " + path_tail(wide(state.workspace), std::max(1, width - 7 - cells(tail))) + tail;
        attrset(A_REVERSE);
        mvhline(height - 1, 0, ' ', width - 1);
        attrset(A_NORMAL);
        put(height - 1, 1, footer, 0, true);
        bool cursor_visible = input_focus && cursor_line >= editor_top && cursor_line < editor_top + input_height;
        curs_set(cursor_visible ? 1 : 0);
        if (cursor_visible)
            move(input_row + cursor_line - editor_top, std::min(width - 2, 6 + cursor_column));
        refresh();
        auto event = terminal.read();
        wint_t key = event.key;
        int kind = event.kind;
        if (kind == ERR)
            continue;
        hint.clear();
        if (kind != KEY_CODE_YES && (key == 3 || (key == 4 && input.empty())))
            break;
        if (kind == KEY_CODE_YES && (key == PasteBegin || key == PasteEnd)) {
            pasting = key == PasteBegin; paste_cr = false; input_focus = true; ensure_cursor = true;
            continue;
        }
        if (pasting) {
            if (kind != KEY_CODE_YES && (std::iswprint(key) || key == '\n' || key == '\r' || key == '\t')) {
                if (key == '\n' && paste_cr) { paste_cr = false; continue; }
                paste_cr = key == '\r';
                input.insert(input.begin() + static_cast<std::ptrdiff_t>(cursor++), key == '\r' ? L'\n' : static_cast<wchar_t>(key));
                ensure_cursor = true;
            }
            continue;
        }
        auto select_tool = [&](int step) {
            std::vector<int> tools;
            for (int i = 0; i < static_cast<int>(state.entries.size()); ++i)
                if (state.entries[i].kind == UiEntry::Tool)
                    tools.push_back(i);
            if (tools.empty()) {
                hint = "当前没有工具调用";
                return;
            }
            auto at = std::find(tools.begin(), tools.end(), selected);
            int pos = at == tools.end() ? static_cast<int>(tools.size()) - 1 : (static_cast<int>(at - tools.begin()) + step + static_cast<int>(tools.size())) % static_cast<int>(tools.size());
            selected = tools[pos];
            input_focus = false;
            following = false;
            ensure_selected = true;
        };
        if (kind == KEY_CODE_YES && key == KEY_MOUSE) {
                if (event.x < 0 || event.x >= width || event.y < 0 || event.y >= height) continue;
                if (event.scroll) {
                    if (event.y >= input_row && event.y < input_row + input_height)
                        editor_top = std::clamp(editor_top + event.scroll, 0, static_cast<int>(editor.size()) - input_height);
                    else {
                        top = std::clamp(top + event.scroll, 0, maximum);
                        following = top == maximum;
                    }
                } else if (event.click) {
                    int index = top + event.y - 1;
                    if (event.y >= 1 && event.y <= body && index < static_cast<int>(rows.size()) && rows[index].header) {
                        selected = rows[index].entry;
                        state.toggle(selected);
                        input_focus = false;
                        following = false;
                        ensure_selected = true;
                    } else if (event.y >= input_row && event.y < input_row + input_height) {
                        input_focus = true;
                        cursor = editor_position(editor[editor_top + event.y - input_row], std::max(0, event.x - 6));
                        ensure_cursor = true;
                    }
                }
        } else if ((kind != KEY_CODE_YES && key == 9) || (kind == KEY_CODE_YES && key == KEY_BTAB))
            select_tool(key == KEY_BTAB ? -1 : 1);
        else if (kind == KEY_CODE_YES && (key == KEY_PPAGE || key == KEY_NPAGE)) {
            if (input_focus && static_cast<int>(editor.size()) > input_height)
                editor_top = std::clamp(editor_top + (key == KEY_PPAGE ? -input_height : input_height), 0, static_cast<int>(editor.size()) - input_height);
            else {
                top = std::clamp(top + (key == KEY_PPAGE ? -body : body), 0, maximum);
                following = top == maximum;
            }
        } else if (!input_focus && kind == KEY_CODE_YES && (key == KEY_UP || key == KEY_DOWN))
            select_tool(key == KEY_UP ? -1 : 1);
        else if (kind != KEY_CODE_YES && key == 27) {
            input_focus = true;
            ensure_cursor = true;
        }
        else if (input_focus && ((kind == KEY_CODE_YES && key == Newline) || (kind != KEY_CODE_YES && key == 14))) {
            input.insert(input.begin() + static_cast<std::ptrdiff_t>(cursor++), L'\n'); ensure_cursor = true;
        }
        else if ((kind != KEY_CODE_YES && (key == 10 || key == 13)) || (kind == KEY_CODE_YES && key == KEY_ENTER)) {
            if (!input_focus) {
                state.toggle(selected);
                following = false;
                ensure_selected = true;
            } else if (input == L"/quit")
                break;
            else if (state.busy)
                hint = "本轮仍在进行，可以先输入，结束后再发送";
            else if (!connected)
                hint = "连接已关闭，请重新启动客户端";
            else if (!input.empty()) {
                auto text = narrow(input);
                if (text.size() > 64 * 1024) {
                    hint = "输入超过 64 KiB";
                    continue;
                }
                auto id = std::to_string(++sequence);
                send_frame(socket, {{"type", "user_message"}, {"request_id", id}, {"text", text}});
                state.ask(text, id);
                input.clear();
                cursor = 0; editor_top = 0; ensure_cursor = true;
                selected = -1;
                following = true;
            }
        } else if (input_focus) {
            ensure_cursor = true;
            if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) || (kind != KEY_CODE_YES && (key == 127 || key == 8))) {
                if (cursor) {
                    input.erase(cursor - 1, 1);
                    --cursor;
                }
            } else if (kind == KEY_CODE_YES && key == KEY_DC) {
                if (cursor < input.size())
                    input.erase(cursor, 1);
            } else if (kind == KEY_CODE_YES && key == KEY_LEFT) {
                if (cursor)
                    --cursor;
            } else if (kind == KEY_CODE_YES && key == KEY_RIGHT) {
                if (cursor < input.size())
                    ++cursor;
            } else if (kind == KEY_CODE_YES && (key == KEY_UP || key == KEY_DOWN)) {
                int line = std::clamp(cursor_line + (key == KEY_UP ? -1 : 1), 0, static_cast<int>(editor.size()) - 1);
                cursor = editor_position(editor[line], cursor_column);
            } else if (kind == KEY_CODE_YES && key == KEY_HOME) cursor = editor[cursor_line].begin;
            else if (kind == KEY_CODE_YES && key == KEY_END) cursor = editor[cursor_line].end;
            else if (kind != KEY_CODE_YES && key == 1)
                cursor = 0;
            else if (kind != KEY_CODE_YES && key == 5)
                cursor = input.size();
            else if (kind != KEY_CODE_YES && key == 21) {
                input.clear();
                cursor = 0; editor_top = 0;
            } else if (kind != KEY_CODE_YES && std::iswprint(key)) {
                input.insert(input.begin() + static_cast<std::ptrdiff_t>(cursor), static_cast<wchar_t>(key));
                ++cursor;
            }
        }
    }
    if (connected && !state.busy)
        try {
            send_frame(socket, {{"type", "quit"}});
        } catch (...) {
        }
    shutdown(socket, SHUT_RDWR);
    return state.exit_code;
}
} // namespace ca
