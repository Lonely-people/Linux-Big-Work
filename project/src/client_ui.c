#include "client_ui.h"
#include "client_input.h"
#include "protocol.h"
#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

typedef struct { unsigned long id; long long updated; int busy; char *title; } entry_t;
typedef struct { char role; int saved; char *text; size_t length; } message_t;
typedef struct { char **items; size_t count; } lines_t;
enum { IDLE, LISTING, OPENING, LEAVING, REPLYING };
typedef struct {
    int fd, operation, chatting, got_begin, history_message, navigation_paste;
    unsigned long id;
    char *title;
    entry_t *entries;
    size_t count, selected;
    message_t *messages;
    size_t message_count, turn_start;
    int columns, rows, scroll, history_height;
    char status[256];
    char frame[PROTOCOL_MAX_LINE];
    size_t frame_length;
    client_editor_t *editor;
} ui_t;

static volatile sig_atomic_t stopped, window_changed;
static struct termios saved_termios;
static int active;

static void ui_signal(int sig) {
    if (sig == SIGWINCH) window_changed = 1;
    else stopped = 1;
}

static void terminal_restore(void) {
    if (active) {
        fputs("\033[0m\033[?25h\033[?2004l\033[?1049l", stdout);
        fflush(stdout);
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        active = 0;
    }
}

int client_ui_available(void) {
    const char *term = getenv("TERM");
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && (!term || strcmp(term, "dumb") != 0);
}

static void free_lines(lines_t *lines) {
    for (size_t i = 0; i < lines->count; ++i) free(lines->items[i]);
    free(lines->items); memset(lines, 0, sizeof(*lines));
}

static int add_line(lines_t *lines) {
    char *empty = strdup("");
    if (!empty) return -1;
    char **items = realloc(lines->items, (lines->count + 1) * sizeof(*items));
    if (!items) { free(empty); return -1; }
    lines->items = items; lines->items[lines->count++] = empty; return 0;
}

static int append_line(lines_t *lines, const char *text, size_t count) {
    char **last = &lines->items[lines->count - 1];
    size_t old = strlen(*last);
    char *resized = realloc(*last, old + count + 1);
    if (!resized) return -1;
    memcpy(resized + old, text, count); resized[old + count] = 0; *last = resized;
    return 0;
}

/* Only printable UTF-8 reaches the terminal. Newlines are layout instructions,
 * tabs are spaces, invalid bytes/control characters are visibly replaced. */
static int wrap_text(lines_t *lines, const char *text, int width) {
    if (add_line(lines) < 0) return -1;
    int column = 0;
    size_t text_length = strlen(text);
    for (size_t at = 0; at < text_length;) {
        if (text[at] == '\n') {
            if (add_line(lines) < 0) return -1;
            column = 0; ++at; continue;
        }
        mbstate_t state = {0}; wchar_t value;
        size_t count = mbrtowc(&value, text + at, text_length - at, &state);
        const char *display = text + at;
        int cells;
        if (count == (size_t)-1 || count == (size_t)-2 || count == 0) {
            count = 1; cells = 1; display = "?";
        } else {
            cells = wcwidth(value);
            if (value == '\t') { display = " "; cells = 1; }
            else if (cells < 0 || value == 27) { display = "?"; cells = 1; }
            /* A standalone combining mark must not modify the UI's prefix. */
            else if (cells == 0 && column == 0) { display = "?"; cells = 1; }
        }
        if (cells > width) { display = "?"; cells = 1; }
        if (column + cells > width) {
            if (add_line(lines) < 0) return -1;
            column = 0;
        }
        size_t display_count = display == text + at ? count : 1;
        if (append_line(lines, display, display_count) < 0) return -1;
        column += cells; at += count;
    }
    return 0;
}

static int display_at(int row, const char *text, int width) {
    lines_t lines = {0};
    if (wrap_text(&lines, text, width) < 0) { free_lines(&lines); return -1; }
    printf("\033[%d;1H%s", row, lines.items[0]);
    free_lines(&lines); return 0;
}

static void clear_messages(ui_t *ui) {
    for (size_t i = 0; i < ui->message_count; ++i) free(ui->messages[i].text);
    free(ui->messages); ui->messages = NULL; ui->message_count = 0;
    ui->scroll = 0; ui->history_message = 0;
}

static void clear_entries(ui_t *ui) {
    for (size_t i = 0; i < ui->count; ++i) free(ui->entries[i].title);
    free(ui->entries); ui->entries = NULL; ui->count = ui->selected = 0;
}

static int new_message(ui_t *ui, char role, int saved) {
    char *text = strdup("");
    if (!text) return -1;
    message_t *messages = realloc(ui->messages, (ui->message_count + 1) * sizeof(*messages));
    if (!messages) { free(text); return -1; }
    ui->messages = messages;
    ui->messages[ui->message_count++] = (message_t){role, saved, text, 0};
    return 0;
}

static int append_message(ui_t *ui, const char *text) {
    if (!ui->message_count) { errno = EPROTO; return -1; }
    message_t *m = &ui->messages[ui->message_count - 1];
    size_t count = strlen(text);
    if (count > (size_t)-1 - m->length - 1) { errno = EOVERFLOW; return -1; }
    char *resized = realloc(m->text, m->length + count + 1);
    if (!resized) return -1;
    memcpy(resized + m->length, text, count + 1);
    m->text = resized; m->length += count; return 0;
}

static int history_lines(ui_t *ui, lines_t *lines) {
    for (size_t i = 0; i < ui->message_count; ++i) {
        message_t *m = &ui->messages[i];
        const char *role = m->role == 'U' ? "You" : m->role == 'A' ? "AI" : "Legacy text";
        char prefix[100];
        const char *suffix = m->saved == 1 ? "" : m->saved == 2 ? " [legacy / not in context]" :
                             ui->operation == REPLYING && i >= ui->turn_start ? " [receiving / not saved yet]" : " [not saved]";
        snprintf(prefix, sizeof(prefix), "%s%s:", role, suffix);
        size_t first_line = lines->count;
        if (wrap_text(lines, prefix, ui->columns - 1) < 0 ||
            wrap_text(lines, m->text, ui->columns - 1) < 0 || add_line(lines) < 0) return -1;
        /* Color after wrapping; reset every line for scrolling and layout. */
        if (m->role == 'U') {
            for (size_t row = first_line; row < lines->count; ++row) {
                size_t length = strlen(lines->items[row]);
                char *colored = malloc(length + sizeof("\033[36m") + sizeof("\033[0m") - 1);
                if (!colored) return -1;
                sprintf(colored, "\033[36m%s\033[0m", lines->items[row]);
                free(lines->items[row]);
                lines->items[row] = colored;
            }
        }
    }
    return 0;
}

static int draw(ui_t *ui) {
    struct winsize size;
    ui->columns = 80; ui->rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        if (size.ws_col) ui->columns = size.ws_col;
        if (size.ws_row) ui->rows = size.ws_row;
    }
    fputs("\033[?25l\033[H\033[2J", stdout);
    int width = ui->columns > 1 ? ui->columns - 1 : 1;
    if (ui->rows < 6 || ui->columns < 20) {
        if (display_at(1, "Window too small; enlarge it (Ctrl+C exits).", width) < 0) return -1;
        fflush(stdout); return 0;
    }
    char header[512];
    if (!ui->chatting) {
        if (display_at(1, "Conversations | Up/Down: select  Enter: open  N: new  R: refresh  Q: quit", width) < 0) return -1;
        size_t available = (size_t)(ui->rows - 3);
        size_t top = ui->selected >= available ? ui->selected - available + 1 : 0;
        if (!ui->count && display_at(3, "No conversations. Press N to start.", width) < 0) return -1;
        for (size_t i = top; i < ui->count && i < top + available; ++i) {
            entry_t *entry = &ui->entries[i];
            char date[32] = "unknown time";
            time_t timestamp = (time_t)entry->updated;
            struct tm tm;
            if (localtime_r(&timestamp, &tm)) strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
            snprintf(header, sizeof(header), "%c %lu %s %s%s", i == ui->selected ? '>' : ' ',
                     entry->id, date, entry->busy ? "[in use] " : "", entry->title);
            if (display_at(3 + (int)(i - top), header, width) < 0) return -1;
        }
    } else {
        snprintf(header, sizeof(header), "Conversation %lu | %s", ui->id, ui->title ? ui->title : "");
        if (display_at(1, header, width) < 0) return -1;
        int input_height = client_editor_height(ui->editor, ui->columns, ui->rows / 3);
        ui->history_height = ui->rows - input_height - 3;
        lines_t lines = {0};
        if (history_lines(ui, &lines) < 0) { free_lines(&lines); return -1; }
        int maximum = (int)lines.count - ui->history_height;
        if (maximum < 0) maximum = 0;
        if (ui->scroll > maximum) ui->scroll = maximum;
        int top = maximum - ui->scroll;
        for (int i = 0; i < ui->history_height && top + i < (int)lines.count; ++i)
            printf("\033[%d;1H%s", i + 2, lines.items[top + i]);
        free_lines(&lines);
        if (display_at(ui->rows - input_height - 1,
                       ui->operation == REPLYING ? "Receiving reply... PgUp/PgDn: history  Ctrl+C: exit" :
                       "Enter: send  Ctrl+J: newline  PgUp/PgDn: history  /back: list  /quit: exit", width) < 0) return -1;
        const char *notice = client_editor_notice(ui->editor);
        if (display_at(ui->rows, notice ? notice : ui->status, width) < 0) return -1;
        client_editor_draw(ui->editor, ui->rows - input_height, input_height);
        if (ui->operation == IDLE) fputs("\033[?25h", stdout);
    }
    if (!ui->chatting && display_at(ui->rows, ui->status, width) < 0) return -1;
    fflush(stdout); return ferror(stdout) ? -1 : 0;
}

static int request(ui_t *ui, const char *command, int operation) {
    if (protocol_send_all(ui->fd, command, strlen(command)) < 0) return -1;
    ui->operation = operation;
    snprintf(ui->status, sizeof(ui->status), "Loading...");
    if (operation == LISTING) clear_entries(ui);
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->updated != y->updated) return x->updated > y->updated ? -1 : 1;
    return x->id > y->id ? -1 : x->id < y->id;
}

static int handle_frame(ui_t *ui, const char *line) {
    char text[PROTOCOL_MAX_LINE];
    if (strncmp(line, "ERROR ", 6) == 0) {
        if (protocol_unescape(line + 6, text, sizeof(text)) < 0) return -1;
        snprintf(ui->status, sizeof(ui->status), "Error: %.240s", text);
        ui->operation = IDLE; ui->got_begin = 0; return 0;
    }
    if (ui->operation == LISTING) {
        if (!strcmp(line, "SESSIONS_BEGIN")) return 0;
        if (!strcmp(line, "SESSIONS_END")) {
            if (ui->count > 1) qsort(ui->entries, ui->count, sizeof(*ui->entries), compare_entries);
            ui->operation = IDLE; ui->status[0] = 0; return 0;
        }
        entry_t entry; int offset = 0;
        if (sscanf(line, "SESSION %lu %lld %d%n", &entry.id, &entry.updated, &entry.busy, &offset) != 3 ||
            !offset || line[offset] != ' ' || protocol_unescape(line + offset + 1, text, sizeof(text)) < 0) goto invalid;
        entry.title = strdup(text);
        if (!entry.title) return -1;
        entry_t *entries = realloc(ui->entries, (ui->count + 1) * sizeof(*entries));
        if (!entries) { free(entry.title); return -1; }
        ui->entries = entries; ui->entries[ui->count++] = entry; return 0;
    }
    if (ui->operation == OPENING) {
        if (strncmp(line, "OPENED ", 7) == 0) {
            int offset = 0; unsigned long id;
            if (sscanf(line, "OPENED %lu%n", &id, &offset) != 1 || !offset || line[offset] != ' ' ||
                protocol_unescape(line + offset + 1, text, sizeof(text)) < 0) goto invalid;
            char *title = strdup(text); if (!title) return -1;
            free(ui->title); ui->title = title; ui->id = id;
            clear_messages(ui); client_editor_clear(ui->editor); ui->chatting = 1; return 0;
        }
        if (!strcmp(line, "HISTORY_END") && !ui->history_message) {
            ui->operation = IDLE; ui->status[0] = 0; return 0;
        }
        if (strncmp(line, "MESSAGE ", 8) == 0 && !ui->history_message) {
            char role, extra; int context;
            if (sscanf(line, "MESSAGE %c %d %c", &role, &context, &extra) != 2 || !strchr("UAX", role)) goto invalid;
            if (new_message(ui, role, context ? 1 : 2) < 0) return -1;
            ui->history_message = 1; return 0;
        }
        if (!strcmp(line, "MESSAGE_END") && ui->history_message) { ui->history_message = 0; return 0; }
        if (!strncmp(line, "TEXT ", 5) && ui->history_message) {
            if (protocol_unescape(line + 5, text, sizeof(text)) < 0) return -1;
            return append_message(ui, text);
        }
    }
    if (ui->operation == LEAVING && !strcmp(line, "LEFT")) {
        clear_messages(ui); ui->chatting = 0; ui->id = 0;
        return request(ui, "SESSIONS\n", LISTING);
    }
    if (ui->operation == REPLYING) {
        if (!strcmp(line, "BEGIN") && !ui->got_begin) {
            ui->got_begin = 1; return new_message(ui, 'A', 0);
        }
        if (!strncmp(line, "DATA ", 5) && ui->got_begin) {
            if (protocol_unescape(line + 5, text, sizeof(text)) < 0) return -1;
            /* Keep the same old viewport while the reply grows below it. */
            lines_t before = {0}, after = {0};
            if (ui->scroll && history_lines(ui, &before) < 0) { free_lines(&before); return -1; }
            int result = append_message(ui, text);
            if (!result && ui->scroll) {
                if (history_lines(ui, &after) < 0) result = -1;
                else ui->scroll += (int)after.count - (int)before.count;
            }
            free_lines(&before); free_lines(&after); return result;
        }
        if (!strcmp(line, "END") && ui->got_begin) {
            int first_saved_turn = 1;
            for (size_t i = 0; i < ui->turn_start; ++i)
                if (ui->messages[i].saved != 0) first_saved_turn = 0;
            if (first_saved_turn && ui->title && !strcmp(ui->title, "New conversation")) {
                const char *question = ui->messages[ui->turn_start].text;
                size_t bytes = 0, characters = 0;
                while (question[bytes] && question[bytes] != '\n' && question[bytes] != '\r') {
                    if (((unsigned char)question[bytes] & 0xc0) != 0x80 && ++characters > 24) break;
                    ++bytes;
                }
                if (bytes) {
                    char *title = strndup(question, bytes);
                    if (!title) return -1;
                    free(ui->title); ui->title = title;
                }
            }
            for (size_t i = ui->turn_start; i < ui->message_count; ++i) ui->messages[i].saved = 1;
            ui->operation = IDLE; ui->got_begin = 0; ui->status[0] = 0; return 0;
        }
    }
 invalid:
    errno = EPROTO; return -1;
}

/* recv() never waits for the rest of a line; keyboard/resize remain responsive. */
static int receive_frames(ui_t *ui) {
    char bytes[16384];
    ssize_t count = recv(ui->fd, bytes, sizeof(bytes), MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    if (count <= 0) { if (!count) errno = ECONNRESET; return -1; }
    for (ssize_t i = 0; i < count; ++i) {
        if (bytes[i] == '\n') {
            ui->frame[ui->frame_length] = 0;
            if (handle_frame(ui, ui->frame) < 0) return -1;
            ui->frame_length = 0;
        } else {
            if (ui->frame_length + 1 >= sizeof(ui->frame) || bytes[i] == 0) { errno = EPROTO; return -1; }
            ui->frame[ui->frame_length++] = bytes[i];
        }
    }
    return 0;
}

/* Menu and busy-state keys deliberately do not alter the chat editor. */
static int navigation_key(void) {
    unsigned char byte;
    if (read(STDIN_FILENO, &byte, 1) != 1) return 3;
    if (byte != 27) return byte;
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&pfd, 1, 100) <= 0 || read(STDIN_FILENO, &byte, 1) != 1 || (byte != '[' && byte != 'O')) return 0;
    char sequence[32]; size_t used = 0;
    while (used + 1 < sizeof(sequence)) {
        if (poll(&pfd, 1, 100) <= 0 || read(STDIN_FILENO, &byte, 1) != 1) return 0;
        sequence[used++] = (char)byte;
        if (byte >= 0x40 && byte <= 0x7e) break;
    }
    sequence[used] = 0;
    if (!strcmp(sequence, "A")) return 1001;
    if (!strcmp(sequence, "B")) return 1002;
    if (!strcmp(sequence, "5~")) return 1003;
    if (!strcmp(sequence, "6~")) return 1004;
    if (!strcmp(sequence, "200~")) return 1005;
    if (!strcmp(sequence, "201~")) return 1006;
    return 0;
}

static int keyboard(ui_t *ui) {
    if (ui->chatting && ui->operation == IDLE && !ui->navigation_paste) {
        int key = client_editor_read_key(ui->editor);
        if (key == CLIENT_EDITOR_EXIT) { stopped = 1; return 0; }
        if (key == CLIENT_EDITOR_PAGE_UP) ui->scroll += ui->history_height;
        if (key == CLIENT_EDITOR_PAGE_DOWN) {
            ui->scroll -= ui->history_height; if (ui->scroll < 0) ui->scroll = 0;
        }
        if (key != CLIENT_EDITOR_SUBMIT) return 0;
        const char *text = client_editor_text(ui->editor);
        if (!strcmp(text, "/quit")) { stopped = 1; return 0; }
        if (!strcmp(text, "/back")) { client_editor_clear(ui->editor); return request(ui, "LEAVE\n", LEAVING); }
        ui->turn_start = ui->message_count;
        if (new_message(ui, 'U', 0) < 0 || append_message(ui, text) < 0 || protocol_send_query(ui->fd, text) < 0) return -1;
        client_editor_clear(ui->editor);
        ui->scroll = 0; ui->operation = REPLYING; ui->got_begin = 0; ui->status[0] = 0;
        return 0;
    }
    int key = navigation_key();
    if (key == 1005) { ui->navigation_paste = 1; return 0; }
    if (key == 1006) { ui->navigation_paste = 0; return 0; }
    if (ui->navigation_paste && key != 3) return 0;
    if (key == 3 || key == 4 || (!ui->chatting && (key == 'q' || key == 'Q'))) { stopped = 1; return 0; }
    if (ui->chatting) {
        if (key == 1003) ui->scroll += ui->history_height;
        if (key == 1004) { ui->scroll -= ui->history_height; if (ui->scroll < 0) ui->scroll = 0; }
        return 0;
    }
    if (ui->operation != IDLE) return 0;
    if (key == 1001 && ui->selected > 0) --ui->selected;
    if (key == 1002 && ui->selected + 1 < ui->count) ++ui->selected;
    if (key == 'r' || key == 'R') return request(ui, "SESSIONS\n", LISTING);
    if (key == 'n' || key == 'N') return request(ui, "NEW\n", OPENING);
    if (key == '\r' && ui->count) {
        char command[64]; snprintf(command, sizeof(command), "OPEN %lu\n", ui->entries[ui->selected].id);
        return request(ui, command, OPENING);
    }
    return 0;
}

int client_ui_run(int socket_fd) {
    if (!setlocale(LC_CTYPE, "") || MB_CUR_MAX < 4) {
        if (!setlocale(LC_CTYPE, "C.UTF-8")) { errno = EILSEQ; return -1; }
    }
    ui_t ui = {.fd = socket_fd};
    ui.editor = client_editor_create(PROTOCOL_MAX_INPUT);
    if (!ui.editor) return -1;
    int result = -1, installed = 0;
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};
    struct sigaction previous[4], action = {0};
    sigemptyset(&action.sa_mask); action.sa_handler = ui_signal;
    stopped = window_changed = 0;
    for (; installed < 4; ++installed)
        if (sigaction(signals[installed], &action, &previous[installed]) < 0) goto cleanup;
    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0) goto cleanup;
    struct termios raw = saved_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | INLCR | IGNCR | ISTRIP);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (atexit(terminal_restore) != 0) { errno = ENOMEM; goto cleanup; }
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) goto cleanup;
    active = 1;
    fputs("\033[?1049h\033[?2004h", stdout);
    if (request(&ui, "SESSIONS\n", LISTING) < 0 || draw(&ui) < 0) goto cleanup;
    while (!stopped) {
        struct pollfd fds[] = {{.fd = socket_fd, .events = POLLIN}, {.fd = STDIN_FILENO, .events = POLLIN}};
        int pending = client_editor_pending(ui.editor);
        int count = poll(fds, 2, pending ? 0 : 100);
        if (count < 0 && errno != EINTR) goto cleanup;
        if (count > 0 && fds[0].revents && receive_frames(&ui) < 0) goto cleanup;
        if ((pending || (count > 0 && fds[1].revents)) && keyboard(&ui) < 0) goto cleanup;
        if (count > 0 || pending || window_changed) {
            window_changed = 0;
            if (draw(&ui) < 0) goto cleanup;
        }
    }
    (void)protocol_send_quit(socket_fd);
    result = 0;
 cleanup: {
        int error = errno;
        terminal_restore();
        for (int i = 0; i < installed; ++i) (void)sigaction(signals[i], &previous[i], NULL);
        client_editor_destroy(ui.editor); clear_entries(&ui); clear_messages(&ui); free(ui.title);
        errno = error;
    }
    return result;
}
