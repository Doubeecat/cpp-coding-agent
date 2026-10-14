#define _XOPEN_SOURCE_EXTENDED 1
#include "client_ui.h"
#include <curses.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

typedef struct {
    wchar_t *data;
    size_t len, cap;
} Wide;
typedef struct {
    Wide text;
    int entry, color;
    bool header;
    size_t begin, end;
} Row;
typedef struct {
    Row *items;
    size_t count, cap;
} Rows;
enum { MOUSE_LEGACY = KEY_MAX + 1, MOUSE_SGR, PASTE_BEGIN, PASTE_END, NEWLINE };
typedef struct {
    int kind, x, y, scroll;
    wint_t key;
    bool click;
} InputEvent;
typedef struct {
    bool active;
    int mouse_encoding;
    Buffer mouse_bytes;
    int64_t mouse_started;
} Terminal;
typedef struct {
    int socket, top, selected, sequence, editor_top, last_width, last_height;
    int width, height, input_row, input_height, body, maximum, cursor_line, cursor_column;
    bool following, input_focus, connected, ensure_selected, ensure_cursor, pasting, paste_cr;
    size_t cursor;
    Wide input;
    Buffer hint;
    UiState state;
    Terminal terminal;
    FrameInbox inbox;
} Tui;
static int minimum(int a, int b) {
    return a < b ? a : b;
}
static int maximum(int a, int b) {
    return a > b ? a : b;
}
static int clamp(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}
static void wide_free(Wide *text) {
    free(text->data);
    memset(text, 0, sizeof(*text));
}
static int wide_reserve(Wide *text, size_t needed, CaError *error) {
    if (needed <= text->cap)
        return 0;
    size_t cap = text->cap ? text->cap : 32;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(wchar_t))
        return ca_error(error, CA_NOMEM, "宽字符缓冲区溢出");
    wchar_t *data = realloc(text->data, cap * sizeof(*data));
    if (!data)
        return ca_error(error, CA_NOMEM, "终端内存不足");
    text->data = data;
    text->cap = cap;
    return 0;
}
static int wide_append(Wide *text, wchar_t ch, CaError *error) {
    if (wide_reserve(text, text->len + 2, error) < 0)
        return -1;
    text->data[text->len++] = ch;
    text->data[text->len] = 0;
    return 0;
}
static int wide_insert(Wide *text, size_t pos, wchar_t ch, CaError *error) {
    if (pos > text->len || wide_reserve(text, text->len + 2, error) < 0)
        return -1;
    memmove(text->data + pos + 1, text->data + pos, (text->len - pos) * sizeof(wchar_t));
    text->data[pos] = ch;
    text->data[++text->len] = 0;
    return 0;
}
static void wide_erase(Wide *text, size_t pos) {
    if (pos >= text->len)
        return;
    memmove(text->data + pos, text->data + pos + 1, (text->len - pos) * sizeof(wchar_t));
    --text->len;
}
static int from_utf8(Wide *out, const char *text, size_t len, CaError *error) {
    mbstate_t state = {0};
    for (size_t i = 0; i < len;) {
        wchar_t ch;
        size_t n = mbrtowc(&ch, text + i, len - i, &state);
        if (n == (size_t)-2)
            break;
        if (n == (size_t)-1) {
            ch = L'�';
            memset(&state, 0, sizeof(state));
            n = 1;
        }
        if (wide_append(out, ch, error) < 0)
            return -1;
        i += n ? n : 1;
    }
    return 0;
}
static int to_utf8(Buffer *out, const Wide *text, CaError *error) {
    mbstate_t state = {0};
    for (size_t i = 0; i < text->len; ++i) {
        char bytes[MB_LEN_MAX];
        size_t n = wcrtomb(bytes, text->data[i], &state);
        if (n == (size_t)-1)
            return ca_error(error, CA_ERROR, "终端字符编码失败");
        if (buf_append(out, bytes, n, error) < 0)
            return -1;
    }
    return 0;
}
static int cells(const wchar_t *text, size_t len) {
    int used = 0;
    for (size_t i = 0; i < len; ++i)
        used += maximum(0, wcwidth(text[i]));
    return used;
}
static void rows_free(Rows *rows) {
    for (size_t i = 0; i < rows->count; ++i)
        wide_free(&rows->items[i].text);
    free(rows->items);
    memset(rows, 0, sizeof(*rows));
}
static int row_add(Rows *rows, const Wide *text, int entry, int color, bool header, size_t begin,
                   size_t end, CaError *error) {
    if (rows->count == rows->cap) {
        size_t cap = rows->cap ? rows->cap * 2 : 32;
        if (cap > INT_MAX || cap > SIZE_MAX / sizeof(Row))
            return ca_error(error, CA_NOMEM, "界面行数溢出");
        Row *items = realloc(rows->items, cap * sizeof(*items));
        if (!items)
            return ca_error(error, CA_NOMEM, "终端内存不足");
        rows->items = items;
        rows->cap = cap;
    }
    Row row = {.entry = entry, .color = color, .header = header, .begin = begin, .end = end};
    if (wide_reserve(&row.text, text->len + 1, error) < 0)
        return -1;
    if (text->len)
        memcpy(row.text.data, text->data, text->len * sizeof(wchar_t));
    row.text.data[text->len] = 0;
    row.text.len = text->len;
    rows->items[rows->count++] = row;
    return 0;
}
static int text_row(Rows *rows, const char *text, size_t len, int entry, int color, bool header,
                    CaError *error) {
    Wide wide = {0};
    int code = from_utf8(&wide, text, len, error);
    if (!code)
        code = row_add(rows, &wide, entry, color, header, 0, 0, error);
    wide_free(&wide);
    return code;
}
static int wrap_rows(Rows *rows, const char *text, size_t len, int width, int entry,
                     CaError *error) {
    Wide wide = {0}, line = {0};
    int used = 0, code = -1;
    if (from_utf8(&wide, text, len, error) < 0)
        goto done;
    for (size_t i = 0; i < wide.len; ++i) {
        wchar_t ch = wide.data[i];
        if (ch == L'\r')
            continue;
        if (ch == L'\n') {
            if (row_add(rows, &line, entry, 0, false, 0, 0, error) < 0)
                goto done;
            line.len = 0;
            used = 0;
            continue;
        }
        if (ch == L'\t')
            ch = L' ';
        if (ch < 32 || ch == 127)
            ch = L'?';
        int size = maximum(0, wcwidth(ch));
        if (used + size > width && line.len) {
            if (row_add(rows, &line, entry, 0, false, 0, 0, error) < 0)
                goto done;
            line.len = 0;
            used = 0;
        }
        if (wide_append(&line, ch, error) < 0)
            goto done;
        used += size;
    }
    code = row_add(rows, &line, entry, 0, false, 0, 0, error);
done:
    wide_free(&wide);
    wide_free(&line);
    return code;
}
static int editor_lines(Rows *rows, const Wide *input, int width, CaError *error) {
    Wide line = {0};
    size_t begin = 0;
    int used = 0, code = -1;
    for (size_t i = 0; i < input->len; ++i) {
        wchar_t ch = input->data[i];
        if (ch == L'\n') {
            if (row_add(rows, &line, -1, 0, false, begin, i, error) < 0)
                goto done;
            line.len = 0;
            begin = i + 1;
            used = 0;
            continue;
        }
        if (ch == L'\t')
            ch = L' ';
        int size = maximum(0, wcwidth(ch));
        if (used + size > width && line.len) {
            if (row_add(rows, &line, -1, 0, false, begin, i, error) < 0)
                goto done;
            line.len = 0;
            begin = i;
            used = 0;
        }
        if (wide_append(&line, ch, error) < 0)
            goto done;
        used += size;
    }
    code = row_add(rows, &line, -1, 0, false, begin, input->len, error);
done:
    wide_free(&line);
    return code;
}
static size_t editor_position(const Row *line, int column) {
    int used = 0;
    for (size_t i = 0; i < line->text.len; ++i) {
        int size = maximum(0, wcwidth(line->text.data[i]));
        if (used + size > column)
            return line->begin + i;
        used += size;
    }
    return line->end;
}
static int terminal_init(Terminal *terminal, CaError *error) {
    if (!setlocale(LC_ALL, "") || MB_CUR_MAX == 1)
        if (!setlocale(LC_ALL, "C.UTF-8"))
            setlocale(LC_ALL, "en_US.UTF-8");
    if (!initscr())
        return ca_error(error, CA_ERROR, "终端初始化失败");
    terminal->active = true;
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
    define_key("\x1b[M", MOUSE_LEGACY);
    define_key("\x1b[<", MOUSE_SGR);
    define_key("\x1b[200~", PASTE_BEGIN);
    define_key("\x1b[201~", PASTE_END);
    define_key("\x1b\r", NEWLINE);
    define_key("\x1b\n", NEWLINE);
    fputs("\x1b[?1000h\x1b[?1006h\x1b[?2004h", stdout);
    fflush(stdout);
    return 0;
}
static void terminal_destroy(Terminal *terminal) {
    if (terminal->active) {
        fputs("\x1b[?1000l\x1b[?1006l\x1b[?2004l", stdout);
        fflush(stdout);
        curs_set(1);
        endwin();
    }
    buf_free(&terminal->mouse_bytes);
}
static InputEvent mouse_event(unsigned button, unsigned x, unsigned y, bool released) {
    InputEvent event = {.kind = ERR};
    if (!x || !y || x > 100000 || y > 100000 || released)
        return event;
    event.kind = KEY_CODE_YES;
    event.key = KEY_MOUSE;
    event.x = (int)x - 1;
    event.y = (int)y - 1;
    if (button & 64)
        event.scroll = (button & 3) == 0 ? -3 : (button & 3) == 1 ? 3 : 0;
    else
        event.click = (button & 35) == 0;
    return event;
}
static InputEvent terminal_read(Terminal *terminal, CaError *error) {
    InputEvent event = {.kind = ERR};
    if (!terminal->mouse_encoding) {
        event.kind = get_wch(&event.key);
        if (event.kind != KEY_CODE_YES || (event.key != MOUSE_LEGACY && event.key != MOUSE_SGR))
            return event;
        terminal->mouse_encoding = (int)event.key;
        buf_clear(&terminal->mouse_bytes);
        terminal->mouse_started = monotonic_ms();
        event.kind = ERR;
    }
    timeout(0);
    while (terminal->mouse_encoding) {
        int ch = getch();
        if (ch == ERR)
            break;
        if (ch > 255) {
            terminal->mouse_encoding = 0;
            break;
        }
        char byte = (char)ch;
        if (buf_append(&terminal->mouse_bytes, &byte, 1, error) < 0)
            break;
        if (terminal->mouse_encoding == MOUSE_LEGACY && terminal->mouse_bytes.len == 3) {
            const unsigned char *b = (const unsigned char *)terminal->mouse_bytes.data;
            if (b[0] >= 32 && b[1] >= 33 && b[2] >= 33)
                event = mouse_event(b[0] - 32, b[1] - 32, b[2] - 32, false);
            terminal->mouse_encoding = 0;
        } else if (terminal->mouse_encoding == MOUSE_SGR && (ch == 'M' || ch == 'm')) {
            unsigned button, x, y;
            char end;
            int consumed = 0;
            if (sscanf(terminal->mouse_bytes.data, "%u;%u;%u%c%n", &button, &x, &y, &end,
                       &consumed) == 4 &&
                consumed == (int)terminal->mouse_bytes.len)
                event = mouse_event(button, x, y, end == 'm');
            terminal->mouse_encoding = 0;
        } else if (terminal->mouse_encoding == MOUSE_SGR &&
                   (terminal->mouse_bytes.len >= 64 || (ch != ';' && (ch < '0' || ch > '9'))))
            terminal->mouse_encoding = 0;
    }
    if (monotonic_ms() - terminal->mouse_started > 500)
        terminal->mouse_encoding = 0;
    timeout(50);
    return event;
}
static int network_event(void *context, const JVal *event, CaError *error) {
    Tui *tui = context;
    const char *id = json_string(event, "request_id", "");
    if (*id && strcmp(id, buf_text(&tui->state.request_id)))
        return ca_error(error, CA_ERROR, "服务端返回错误的 request_id");
    int code = ui_apply(&tui->state, event, error);
    if (!code && !strcmp(json_string(event, "type", ""), "session_history") &&
        json_bool(event, "done", false)) {
        tui->selected = -1;
        tui->ensure_selected = false;
        tui->following = tui->input_focus = tui->ensure_cursor = true;
    }
    return code;
}
static void put_wide(int row, int col, const wchar_t *text, size_t len, int width, int color,
                     bool reverse) {
    int attr = color ? COLOR_PAIR(color) : A_NORMAL;
    if (reverse)
        attr |= A_REVERSE;
    size_t count = 0;
    int used = 0;
    while (count < len) {
        int size = maximum(0, wcwidth(text[count]));
        if (used + size > width - col - 1)
            break;
        used += size;
        ++count;
    }
    attrset(attr);
    if (count)
        mvaddnwstr(row, col, text, (int)count);
    attrset(A_NORMAL);
}
static int put_text(int row, int col, const char *text, int width, int color, bool reverse,
                    CaError *error) {
    Wide wide = {0};
    int code = from_utf8(&wide, text, strlen(text), error);
    if (!code)
        put_wide(row, col, wide.data, wide.len, width, color, reverse);
    wide_free(&wide);
    return code;
}
static int chat_rows(Tui *tui, Rows *rows, CaError *error) {
    for (size_t i = 0; i < tui->state.count; ++i) {
        UiEntry *entry = &tui->state.entries[i];
        Buffer text = {0};
        int code = 0;
        if (entry->kind == UI_TOOL) {
            int color = entry->running ? 2 : json_bool(JV(entry->result), "ok", false) ? 1 : 3;
            code = ui_summary(&tui->state, (int)i, &text, error);
            if (!code)
                code = text_row(rows, buf_text(&text), text.len, (int)i, color, true, error);
            if (!code && entry->expanded) {
                buf_clear(&text);
                code = ui_details(&tui->state, (int)i, &text, error);
                if (!code)
                    code =
                        wrap_rows(rows, buf_text(&text), text.len, tui->width - 4, (int)i, error);
            }
        } else {
            const char *label = entry->kind == UI_USER        ? "你"
                                : entry->kind == UI_ASSISTANT ? "助手"
                                                              : "提示";
            code = text_row(rows, label, strlen(label), (int)i, entry->kind == UI_NOTICE ? 3 : 1,
                            false, error);
            if (!code)
                code = wrap_rows(rows, buf_text(&entry->text), entry->text.len, tui->width - 4,
                                 (int)i, error);
        }
        buf_free(&text);
        if (code < 0)
            return -1;
        if (text_row(rows, "", 0, -1, 0, false, error) < 0)
            return -1;
    }
    if (!rows->count) {
        const char *intro = "输入需求，开始生成或修改 C 程序。\n工具默认折叠：点击标题，或 Tab "
                            "选中后按 Enter 展开。";
        return wrap_rows(rows, intro, strlen(intro), tui->width - 4, -1, error);
    }
    return 0;
}
static int footer(Tui *tui, CaError *error) {
    Buffer tail = {0}, tokens = {0};
    Wide model = {0}, path = {0}, wide_tail = {0}, line = {0};
    int code = -1;
    if (tui->state.tokens < 0) {
        if (buf_append(&tokens, "—", strlen("—"), error) < 0)
            goto done;
    } else if (buf_printf(&tokens, error, "%d", tui->state.tokens) < 0)
        goto done;
    if (buf_printf(&tail, error, " | Token %s/%d | 模型 ", buf_text(&tokens), tui->state.context) <
            0 ||
        from_utf8(&wide_tail, tail.data, tail.len, error) < 0 ||
        from_utf8(&model, buf_text(&tui->state.model), tui->state.model.len, error) < 0)
        goto done;
    int model_width =
        maximum(1, minimum(32, tui->width - 8 - cells(wide_tail.data, wide_tail.len)));
    int used = 0;
    size_t model_len = 0;
    bool clipped = cells(model.data, model.len) > model_width;
    while (model_len < model.len) {
        int size = maximum(0, wcwidth(model.data[model_len]));
        if (used + size > model_width - (clipped ? 1 : 0))
            break;
        used += size;
        ++model_len;
    }
    for (size_t i = 0; i < model_len; ++i)
        if (wide_append(&wide_tail, model.data[i], error) < 0)
            goto done;
    if (clipped && wide_append(&wide_tail, L'…', error) < 0)
        goto done;
    if (from_utf8(&path, buf_text(&tui->state.workspace), tui->state.workspace.len, error) < 0 ||
        from_utf8(&line, "目录 ", strlen("目录 "), error) < 0)
        goto done;
    int room = maximum(1, tui->width - 7 - cells(wide_tail.data, wide_tail.len));
    size_t start = 0;
    if (cells(path.data, path.len) > room) {
        used = 1;
        start = path.len;
        while (start && used + maximum(0, wcwidth(path.data[start - 1])) <= room) {
            used += maximum(0, wcwidth(path.data[--start]));
        }
        if (wide_append(&line, L'…', error) < 0)
            goto done;
    }
    for (size_t i = start; i < path.len; ++i)
        if (wide_append(&line, path.data[i], error) < 0)
            goto done;
    for (size_t i = 0; i < wide_tail.len; ++i)
        if (wide_append(&line, wide_tail.data[i], error) < 0)
            goto done;
    attrset(A_REVERSE);
    mvhline(tui->height - 1, 0, ' ', tui->width - 1);
    attrset(A_NORMAL);
    put_wide(tui->height - 1, 1, line.data, line.len, tui->width, 0, true);
    code = 0;
done:
    buf_free(&tail);
    buf_free(&tokens);
    wide_free(&model);
    wide_free(&path);
    wide_free(&wide_tail);
    wide_free(&line);
    return code;
}
static int render(Tui *tui, Rows *chat, Rows *editor, CaError *error) {
    tui->cursor_line = 0;
    for (size_t i = 0; i < editor->count; ++i)
        if (editor->items[i].begin <= tui->cursor)
            tui->cursor_line = (int)i;
    Row *cursor_row = &editor->items[tui->cursor_line];
    tui->cursor_column = cells(cursor_row->text.data, tui->cursor - cursor_row->begin);
    tui->input_height = minimum((int)editor->count, maximum(1, tui->height - 8));
    tui->input_row = tui->height - 2 - tui->input_height;
    int separator = tui->input_row - 1;
    if (tui->width != tui->last_width || tui->height != tui->last_height)
        tui->ensure_cursor = true;
    tui->last_width = tui->width;
    tui->last_height = tui->height;
    if (tui->ensure_cursor) {
        if (tui->cursor_line < tui->editor_top)
            tui->editor_top = tui->cursor_line;
        if (tui->cursor_line >= tui->editor_top + tui->input_height)
            tui->editor_top = tui->cursor_line - tui->input_height + 1;
        tui->ensure_cursor = false;
    }
    tui->editor_top = clamp(tui->editor_top, 0, (int)editor->count - tui->input_height);
    Buffer heading = {0};
    int code = -1;
    if (buf_append(&heading, "Coding Agent  · ", strlen("Coding Agent  · "), error) < 0)
        goto done;
    if (tui->state.busy &&
        buf_printf(&heading, error, "%c ", "|/-\\"[(int)(ui_elapsed(&tui->state) * 8) % 4]) < 0)
        goto done;
    if (buf_append(&heading, buf_text(&tui->state.status), tui->state.status.len, error) < 0)
        goto done;
    if (tui->state.request_id.len &&
        buf_printf(&heading, error, " · %.1fs", ui_elapsed(&tui->state)) < 0)
        goto done;
    if (put_text(0, 1, buf_text(&heading), tui->width, 1, false, error) < 0)
        goto done;
    tui->body = separator - 1;
    tui->maximum = maximum(0, (int)chat->count - tui->body);
    if (tui->following)
        tui->top = tui->maximum;
    tui->top = clamp(tui->top, 0, tui->maximum);
    if (tui->ensure_selected) {
        for (size_t i = 0; i < chat->count; ++i)
            if (chat->items[i].header && chat->items[i].entry == tui->selected) {
                if ((int)i < tui->top)
                    tui->top = (int)i;
                if ((int)i >= tui->top + tui->body)
                    tui->top = (int)i - tui->body + 1;
                break;
            }
        tui->top = clamp(tui->top, 0, tui->maximum);
        tui->ensure_selected = false;
    }
    for (int i = 0; i < tui->body && (size_t)(tui->top + i) < chat->count; ++i) {
        Row *row = &chat->items[tui->top + i];
        put_wide(i + 1, row->header ? 1 : 2, row->text.data, row->text.len, tui->width, row->color,
                 row->header && !tui->input_focus && row->entry == tui->selected);
    }
    if (tui->maximum > 0) {
        mvvline(1, tui->width - 1, ACS_VLINE, tui->body);
        int thumb = maximum(1, tui->body * tui->body / (int)chat->count),
            start = tui->top * (tui->body - thumb) / tui->maximum;
        mvvline(1 + start, tui->width - 1, ACS_CKBOARD, thumb);
    }
    mvhline(separator, 0, ACS_HLINE, tui->width);
    if ((int)editor->count > tui->input_height) {
        Buffer info = {0};
        int status = buf_printf(&info, error, "输入 %d–%d/%zu 行", tui->editor_top + 1,
                                tui->editor_top + tui->input_height, editor->count);
        if (!status)
            status = put_text(separator, 2, buf_text(&info), tui->width, 2, false, error);
        buf_free(&info);
        if (status < 0)
            goto done;
    }
    if (put_text(tui->input_row, 1, "你 > ", tui->width, 1, false, error) < 0)
        goto done;
    for (int i = 0; i < tui->input_height; ++i) {
        Row *line = &editor->items[tui->editor_top + i];
        put_wide(tui->input_row + i, 6, line->text.data, line->text.len, tui->width, 0, false);
    }
    const char *help = tui->hint.len ? buf_text(&tui->hint)
                       : tui->input_focus
                           ? "Enter 发送 · Alt+Enter 换行 · Tab 工具 · 滚轮滚动 · Ctrl+C 退出"
                           : "Enter 展开 · ↑↓ 工具 · Esc 输入 · 滚轮/PgUp/PgDn 滚动 · Ctrl+C 退出";
    if (put_text(tui->height - 2, 1, help, tui->width, 2, false, error) < 0 ||
        footer(tui, error) < 0)
        goto done;
    bool visible = tui->input_focus && tui->cursor_line >= tui->editor_top &&
                   tui->cursor_line < tui->editor_top + tui->input_height;
    curs_set(visible ? 1 : 0);
    if (visible)
        move(tui->input_row + tui->cursor_line - tui->editor_top,
             minimum(tui->width - 2, 6 + tui->cursor_column));
    refresh();
    code = 0;
done:
    buf_free(&heading);
    return code;
}
static int hint(Tui *tui, const char *text, CaError *error) {
    buf_clear(&tui->hint);
    return buf_append(&tui->hint, text, strlen(text), error);
}
static int select_tool(Tui *tui, int step, CaError *error) {
    size_t count = tui->state.count;
    int last = -1;
    for (size_t i = 0; i < count; ++i)
        if (tui->state.entries[i].kind == UI_TOOL)
            last = (int)i;
    if (last < 0)
        return hint(tui, "当前没有工具调用", error);
    int selected = last;
    if (tui->selected >= 0 && (size_t)tui->selected < count &&
        tui->state.entries[tui->selected].kind == UI_TOOL) {
        for (size_t offset = 1; offset <= count; ++offset) {
            size_t index = step > 0 ? ((size_t)tui->selected + offset) % count
                                    : ((size_t)tui->selected + count - offset % count) % count;
            if (tui->state.entries[index].kind == UI_TOOL) {
                selected = (int)index;
                break;
            }
        }
    }
    tui->selected = selected;
    tui->input_focus = false;
    tui->following = false;
    tui->ensure_selected = true;
    return 0;
}
static int send_request(Tui *tui, Json *request, CaError *error) {
    int code = request && !request->failed ? send_frame(tui->socket, JV(request), 10000, error)
                                           : ca_error(error, CA_NOMEM, "内存不足");
    json_free(request);
    return code;
}
static void clear_editor(Tui *tui) {
    tui->input.len = 0;
    if (tui->input.data)
        tui->input.data[0] = 0;
    tui->cursor = 0;
    tui->editor_top = 0;
    tui->ensure_cursor = true;
    tui->following = true;
}
static int submit(Tui *tui, CaError *error) {
    if (tui->state.busy)
        return hint(tui, "本轮仍在进行，可以先输入，结束后再发送", error);
    if (tui->state.restoring)
        return hint(tui, "正在恢复会话，完成后再发送", error);
    if (!tui->connected)
        return hint(tui, "连接已关闭，请重新启动客户端", error);
    Buffer text = {0};
    int code = -1;
    if (to_utf8(&text, &tui->input, error) < 0)
        goto done;
    if (!strcmp(buf_text(&text), "/quit")) {
        code = 1;
        goto done;
    }
    char *space = strchr(text.data ? text.data : "", ' ');
    size_t name_len = space ? (size_t)(space - text.data) : text.len;
    const char *argument = space ? space + 1 : "";
    while (*argument == ' ')
        ++argument;
    bool sessions = name_len == 9 && !memcmp(buf_text(&text), "/sessions", 9);
    bool resume = name_len == 7 && !memcmp(buf_text(&text), "/resume", 7);
    bool directory = name_len == 4 && !memcmp(buf_text(&text), "/dir", 4);
    if (sessions || resume || directory) {
        Json *request = NULL;
        Buffer notice = {0};
        if (sessions) {
            request = json_event("list_sessions");
            code = buf_append(&notice, "正在读取会话列表…", strlen("正在读取会话列表…"), error);
        } else if (resume) {
            if (!*argument)
                code = hint(tui, "用法：/resume <会话 ID>，先用 /sessions 查看", error);
            else {
                request = json_event("resume_session");
                json_set_string(request, "session_id", argument);
                json_set_bool(request, "include_history", true);
                code = buf_printf(&notice, error, "正在恢复会话 %s…", argument);
            }
        } else if (!*argument) {
            buf_clear(&tui->hint);
            code = buf_printf(&tui->hint, error, "当前目录：%s", buf_text(&tui->state.workspace));
        } else {
            char *joined = *argument == '/'
                               ? ca_strdup(argument, error)
                               : path_join(buf_text(&tui->state.workspace), argument, error);
            char *path = joined ? path_absolute(joined, error) : NULL;
            free(joined);
            if (path) {
                request = json_event("set_workspace");
                json_set_string(request, "workspace", path);
                free(path);
                code = buf_append(&notice, "正在切换目录…", strlen("正在切换目录…"), error);
            }
        }
        if (request) {
            if (code < 0)
                json_free(request);
            else {
                code = send_request(tui, request, error);
                if (!code)
                    code = ui_add(&tui->state, UI_NOTICE, buf_text(&notice), notice.len, error);
                if (!code && resume)
                    tui->state.restoring = true;
            }
        }
        buf_free(&notice);
        if (!code)
            clear_editor(tui);
        goto done;
    }
    if (text.len > 64 * 1024) {
        code = hint(tui, "输入超过 64 KiB", error);
        goto done;
    }
    if (!text.len) {
        code = 0;
        goto done;
    }
    char id[24];
    snprintf(id, sizeof(id), "%d", ++tui->sequence);
    Json *request = json_event("user_message");
    json_set_string(request, "request_id", id);
    json_set_stringn(request, "text", text.data, text.len);
    if (send_request(tui, request, error) < 0 ||
        ui_ask(&tui->state, text.data, text.len, id, error) < 0)
        goto done;
    clear_editor(tui);
    tui->selected = -1;
    code = 0;
done:
    buf_free(&text);
    return code;
}
static int mouse_input(Tui *tui, const InputEvent *event, const Rows *chat, const Rows *editor) {
    if (event->x < 0 || event->x >= tui->width || event->y < 0 || event->y >= tui->height)
        return 0;
    if (event->scroll) {
        if (event->y >= tui->input_row && event->y < tui->input_row + tui->input_height)
            tui->editor_top =
                clamp(tui->editor_top + event->scroll, 0, (int)editor->count - tui->input_height);
        else {
            tui->top = clamp(tui->top + event->scroll, 0, tui->maximum);
            tui->following = tui->top == tui->maximum;
        }
    } else if (event->click) {
        int index = tui->top + event->y - 1;
        if (event->y >= 1 && event->y <= tui->body && (size_t)index < chat->count &&
            chat->items[index].header) {
            tui->selected = chat->items[index].entry;
            ui_toggle(&tui->state, tui->selected);
            tui->input_focus = false;
            tui->following = false;
            tui->ensure_selected = true;
        } else if (event->y >= tui->input_row && event->y < tui->input_row + tui->input_height) {
            tui->input_focus = true;
            tui->cursor =
                editor_position(&editor->items[tui->editor_top + event->y - tui->input_row],
                                maximum(0, event->x - 6));
            tui->ensure_cursor = true;
        }
    }
    return 0;
}
static int edit_input(Tui *tui, const InputEvent *event, const Rows *editor, CaError *error) {
    wint_t key = event->key;
    int kind = event->kind;
    tui->ensure_cursor = true;
    if ((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
        (kind != KEY_CODE_YES && (key == 127 || key == 8))) {
        if (tui->cursor)
            wide_erase(&tui->input, --tui->cursor);
    } else if (kind == KEY_CODE_YES && key == KEY_DC)
        wide_erase(&tui->input, tui->cursor);
    else if (kind == KEY_CODE_YES && key == KEY_LEFT) {
        if (tui->cursor)
            --tui->cursor;
    } else if (kind == KEY_CODE_YES && key == KEY_RIGHT) {
        if (tui->cursor < tui->input.len)
            ++tui->cursor;
    } else if (kind == KEY_CODE_YES && (key == KEY_UP || key == KEY_DOWN)) {
        int line = clamp(tui->cursor_line + (key == KEY_UP ? -1 : 1), 0, (int)editor->count - 1);
        tui->cursor = editor_position(&editor->items[line], tui->cursor_column);
    } else if (kind == KEY_CODE_YES && key == KEY_HOME)
        tui->cursor = editor->items[tui->cursor_line].begin;
    else if (kind == KEY_CODE_YES && key == KEY_END)
        tui->cursor = editor->items[tui->cursor_line].end;
    else if (kind != KEY_CODE_YES && key == 1)
        tui->cursor = 0;
    else if (kind != KEY_CODE_YES && key == 5)
        tui->cursor = tui->input.len;
    else if (kind != KEY_CODE_YES && key == 21)
        clear_editor(tui);
    else if (kind != KEY_CODE_YES && iswprint(key)) {
        if (wide_insert(&tui->input, tui->cursor, (wchar_t)key, error) < 0)
            return -1;
        ++tui->cursor;
    }
    return 0;
}
static int handle_input(Tui *tui, InputEvent event, const Rows *chat, const Rows *editor,
                        CaError *error) {
    wint_t key = event.key;
    int kind = event.kind;
    if (kind == ERR)
        return 0;
    buf_clear(&tui->hint);
    if (kind != KEY_CODE_YES && (key == 3 || (key == 4 && !tui->input.len)))
        return 1;
    if (kind == KEY_CODE_YES && (key == PASTE_BEGIN || key == PASTE_END)) {
        tui->pasting = key == PASTE_BEGIN;
        tui->paste_cr = false;
        tui->input_focus = true;
        tui->ensure_cursor = true;
        return 0;
    }
    if (tui->pasting) {
        if (kind != KEY_CODE_YES && (iswprint(key) || key == '\n' || key == '\r' || key == '\t')) {
            if (key == '\n' && tui->paste_cr) {
                tui->paste_cr = false;
                return 0;
            }
            tui->paste_cr = key == '\r';
            if (wide_insert(&tui->input, tui->cursor, key == '\r' ? L'\n' : (wchar_t)key, error) <
                0)
                return -1;
            ++tui->cursor;
            tui->ensure_cursor = true;
        }
        return 0;
    }
    if (kind == KEY_CODE_YES && key == KEY_MOUSE)
        return mouse_input(tui, &event, chat, editor);
    if ((kind != KEY_CODE_YES && key == 9) || (kind == KEY_CODE_YES && key == KEY_BTAB))
        return select_tool(tui, key == KEY_BTAB ? -1 : 1, error);
    if (kind == KEY_CODE_YES && (key == KEY_PPAGE || key == KEY_NPAGE)) {
        if (tui->input_focus && (int)editor->count > tui->input_height)
            tui->editor_top =
                clamp(tui->editor_top + (key == KEY_PPAGE ? -tui->input_height : tui->input_height),
                      0, (int)editor->count - tui->input_height);
        else {
            tui->top =
                clamp(tui->top + (key == KEY_PPAGE ? -tui->body : tui->body), 0, tui->maximum);
            tui->following = tui->top == tui->maximum;
        }
    } else if (!tui->input_focus && kind == KEY_CODE_YES && (key == KEY_UP || key == KEY_DOWN))
        return select_tool(tui, key == KEY_UP ? -1 : 1, error);
    else if (kind != KEY_CODE_YES && key == 27) {
        tui->input_focus = true;
        tui->ensure_cursor = true;
    } else if (tui->input_focus &&
               ((kind == KEY_CODE_YES && key == NEWLINE) || (kind != KEY_CODE_YES && key == 14))) {
        if (wide_insert(&tui->input, tui->cursor, L'\n', error) < 0)
            return -1;
        ++tui->cursor;
        tui->ensure_cursor = true;
    } else if ((kind != KEY_CODE_YES && (key == 10 || key == 13)) ||
               (kind == KEY_CODE_YES && key == KEY_ENTER)) {
        if (!tui->input_focus) {
            ui_toggle(&tui->state, tui->selected);
            tui->following = false;
            tui->ensure_selected = true;
        } else
            return submit(tui, error);
    } else if (tui->input_focus)
        return edit_input(tui, &event, editor, error);
    return 0;
}
int run_tui(int socket, UiState *initial, CaError *error) {
    Tui tui = {.socket = socket,
               .selected = -1,
               .following = true,
               .input_focus = true,
               .connected = true,
               .ensure_cursor = true};
    tui.state = *initial;
    memset(initial, 0, sizeof(*initial));
    int code = -1;
    if (terminal_init(&tui.terminal, error) < 0)
        goto done;
    while (!stopping) {
        struct pollfd net = {socket, POLLIN, 0};
        if (tui.connected && poll(&net, 1, 0) > 0) {
            int received = inbox_receive(&tui.inbox, socket, network_event, &tui, error);
            if (received < 0)
                goto done;
            tui.connected = received != 0;
            if (!tui.connected) {
                Json *event = json_event("task_done");
                json_set_string(event, "status", "disconnected");
                int status = ui_apply(&tui.state, JV(event), error);
                json_free(event);
                if (status < 0)
                    goto done;
                buf_clear(&tui.state.status);
                if (buf_append(&tui.state.status, "连接已关闭", strlen("连接已关闭"), error) < 0)
                    goto done;
            }
        }
        getmaxyx(stdscr, tui.height, tui.width);
        erase();
        if (tui.height < 8 || tui.width < 45) {
            mvaddstr(0, 0, "Terminal too small (45x8). Ctrl+C to exit.");
            refresh();
            InputEvent event = terminal_read(&tui.terminal, error);
            if (error->code)
                goto done;
            if (event.kind != ERR && (event.key == 3 || event.key == 4))
                break;
            continue;
        }
        Rows chat = {0}, editor = {0};
        int status = chat_rows(&tui, &chat, error);
        if (!status)
            status = editor_lines(&editor, &tui.input, tui.width - 8, error);
        if (!status)
            status = render(&tui, &chat, &editor, error);
        if (!status) {
            InputEvent event = terminal_read(&tui.terminal, error);
            status = error->code ? -1 : handle_input(&tui, event, &chat, &editor, error);
        }
        rows_free(&chat);
        rows_free(&editor);
        if (status < 0)
            goto done;
        if (status > 0)
            break;
    }
    code = tui.state.exit_code;
done:
    if (tui.connected && !tui.state.busy && !stopping) {
        Json *quit = json_event("quit");
        CaError ignored = {0};
        send_frame(socket, JV(quit), 1000, &ignored);
        json_free(quit);
    }
    shutdown(socket, SHUT_RDWR);
    terminal_destroy(&tui.terminal);
    ui_destroy(&tui.state);
    wide_free(&tui.input);
    buf_free(&tui.hint);
    buf_free(&tui.inbox.bytes);
    return code;
}
