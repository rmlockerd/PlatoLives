#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>

#include "plato/plato_terminal.h"
#include "plato/plato_transport.h"
#include "plato/plato_protocol.h"
#include "plato/plato_keyboard.h"
#include "plato/plato_ringbuf.h"
#include "plato/console_runner.h"

static struct termios g_orig_termios;
static bool g_raw_enabled = false;
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_resized = 1;
static volatile sig_atomic_t g_status_dirty = 1;
static double g_feedback_until = 0.0;
static char g_feedback_msg[64] = "";
static plato_console_clipboard_cb g_clipboard_cb = NULL;

void plato_console_set_clipboard_callback(plato_console_clipboard_cb cb) {
    g_clipboard_cb = cb;
}

static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void show_status_feedback(const char *msg, double duration_sec) {
    snprintf(g_feedback_msg, sizeof(g_feedback_msg), "%s", msg);
    g_feedback_until = get_time_sec() + duration_sec;
    g_status_dirty = 1;
}

static void console_restore_terminal(void) {
    if (g_raw_enabled) {
        write(STDOUT_FILENO, "\033[0m\033[?25h\033[?1049l", 19);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_enabled = false;
    }
}

static void console_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) {
        g_quit = 1;
    } else if (sig == SIGWINCH) {
        g_resized = 1;
    }
}

static bool console_enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) return false;

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return false;
    g_raw_enabled = true;
    atexit(console_restore_terminal);

    write(STDOUT_FILENO, "\033[?1049h\033[?25l\033[2J\033[H", 20);
    return true;
}

static void console_beep_callback(void *context) {
    (void)context;
    write(STDOUT_FILENO, "\a", 1);
}

static void console_metadata_callback(void *context, const char *name, const char *group, const char *system, const char *station) {
    (void)context; (void)name; (void)group; (void)system; (void)station;
    g_status_dirty = 1;
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void send_osc52_copy(const char *text, size_t len) {
    write(STDOUT_FILENO, "\033]52;c;", 7);
    char chunk[4];
    for (size_t i = 0; i < len; i += 3) {
        uint32_t val = (uint8_t)text[i] << 16;
        if (i + 1 < len) val |= (uint8_t)text[i + 1] << 8;
        if (i + 2 < len) val |= (uint8_t)text[i + 2];

        chunk[0] = b64_table[(val >> 18) & 0x3F];
        chunk[1] = b64_table[(val >> 12) & 0x3F];
        chunk[2] = (i + 1 < len) ? b64_table[(val >> 6) & 0x3F] : '=';
        chunk[3] = (i + 2 < len) ? b64_table[val & 0x3F] : '=';
        write(STDOUT_FILENO, chunk, 4);
    }
    write(STDOUT_FILENO, "\a", 1);
}

static void copy_screen_to_clipboard(const plato_terminal_t *term) {
    char buf[16384];
    size_t out_len = 0;
    char tmp[8];

    for (int r = 0; r < PLATO_ROWS; r++) {
        for (int c = 0; c < PLATO_COLS; c++) {
            const char *utf = plato_cell_to_utf8(term, term->text_grid[r][c], tmp);
            size_t ulen = strlen(utf);
            if (out_len + ulen < sizeof(buf) - 2) {
                memcpy(&buf[out_len], utf, ulen);
                out_len += ulen;
            }
        }
        if (r < PLATO_ROWS - 1) {
            buf[out_len++] = '\n';
        }
    }
    buf[out_len] = '\0';

    FILE *f = fopen("screen.txt", "w");
    if (f) {
        fwrite(buf, 1, out_len, f);
        fclose(f);
    }

    if (g_clipboard_cb) {
        g_clipboard_cb(buf, out_len);
    }

    send_osc52_copy(buf, out_len);
    show_status_feedback("*** SCHERMO SALVATO IN screen.txt E NEGLI APPUNTI ***", 1.5);
}

typedef struct {
    plato_special_key_t key;
    double start_time;
    double expiry;
    bool active;
} pending_key_t;

static pending_key_t g_pending = { PLATO_KEY_NONE, 0.0, 0.0, false };

static void flush_pending_key(plato_terminal_t *term) {
    if (g_pending.active) {
        plato_protocol_send_key(term, plato_keyboard_keycode(g_pending.key, false));
        g_pending.active = false;
    }
}

static void trigger_key_double_tap(plato_terminal_t *term, plato_special_key_t key, bool explicit_shift) {
    double now = get_time_sec();

    if (explicit_shift) {
        flush_pending_key(term);
        plato_protocol_send_key(term, plato_keyboard_keycode(key, true));
        return;
    }

    if (g_pending.active && g_pending.key == key && (now - g_pending.start_time) < 0.06) {
        return;
    }

    if (g_pending.active && g_pending.key == key && now < g_pending.expiry) {
        g_pending.active = false;
        plato_protocol_send_key(term, plato_keyboard_keycode(key, true));

        const char *name = "COMANDO";
        switch (key) {
            case PLATO_KEY_NEXT: name = "SHIFT-NEXT"; break;
            case PLATO_KEY_STOP: name = "SHIFT-STOP"; break;
            case PLATO_KEY_BACK: name = "SHIFT-BACK"; break;
            case PLATO_KEY_HELP: name = "SHIFT-HELP"; break;
            case PLATO_KEY_LAB:  name = "SHIFT-LAB"; break;
            case PLATO_KEY_DATA: name = "SHIFT-DATA"; break;
            case PLATO_KEY_EDIT: name = "SHIFT-EDIT"; break;
            default: break;
        }
        char fb[64];
        snprintf(fb, sizeof(fb), "*** [ %s INVIATO ] ***", name);
        show_status_feedback(fb, 1.2);
        return;
    }

    if (g_pending.active) {
        flush_pending_key(term);
    }

    g_pending.key = key;
    g_pending.start_time = now;
    g_pending.expiry = now + 0.50;
    g_pending.active = true;
}

typedef struct {
    plato_transport_t *transport;
    plato_ringbuf_t *ringbuf;
    volatile bool running;
} console_net_worker_ctx_t;

static void* console_net_worker(void *arg) {
    console_net_worker_ctx_t *ctx = (console_net_worker_ctx_t *)arg;
    uint8_t buf[4096];
    while (ctx->running) {
        if (ctx->transport && ctx->transport->connected) {
            int n = plato_transport_recv(ctx->transport, buf, sizeof(buf));
            if (n > 0) {
                plato_ringbuf_write(ctx->ringbuf, buf, (size_t)n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                plato_transport_disconnect(ctx->transport);
                usleep(30000);
            }
        } else {
            usleep(20000);
        }
    }
    return NULL;
}

static void draw_bezel(int offset_x, int offset_y, int term_w, int term_h) {
    if (offset_x < 2 || offset_y < 2 || (offset_x + PLATO_COLS) >= term_w || (offset_y + PLATO_ROWS) >= term_h) {
        return;
    }
    const char *dim_amber = "\033[38;2;120;65;0m";
    write(STDOUT_FILENO, dim_amber, strlen(dim_amber));

    char buf[512];
    int top_r = offset_y - 1;
    int bot_r = offset_y + PLATO_ROWS;
    int left_c = offset_x - 1;
    int right_c = offset_x + PLATO_COLS;

    int len = snprintf(buf, sizeof(buf), "\033[%d;%dH+", top_r, left_c);
    write(STDOUT_FILENO, buf, len);
    for (int c = 0; c < PLATO_COLS; c++) write(STDOUT_FILENO, "-", 1);
    write(STDOUT_FILENO, "+", 1);

    for (int r = offset_y; r < offset_y + PLATO_ROWS; r++) {
        len = snprintf(buf, sizeof(buf), "\033[%d;%dH|", r, left_c);
        write(STDOUT_FILENO, buf, len);
        len = snprintf(buf, sizeof(buf), "\033[%d;%dH|", r, right_c);
        write(STDOUT_FILENO, buf, len);
    }

    len = snprintf(buf, sizeof(buf), "\033[%d;%dH+", bot_r, left_c);
    write(STDOUT_FILENO, buf, len);
    for (int c = 0; c < PLATO_COLS; c++) write(STDOUT_FILENO, "-", 1);
    write(STDOUT_FILENO, "+", 1);

    write(STDOUT_FILENO, "\033[0m", 4);
}

static void render_status_bar(const plato_terminal_t *term, const char *host, int port, int term_w, int status_y) {
    char bar[512];

    if (get_time_sec() < g_feedback_until) {
        snprintf(bar, sizeof(bar), " PLATO | %s", g_feedback_msg);
    } else {
        char user_info[96] = "";
        if (term->user_name[0] && term->user_group[0]) {
            if (term->user_station[0]) {
                snprintf(user_info, sizeof(user_info), "User: %s/%s (%s) | ", term->user_name, term->user_group, term->user_station);
            } else {
                snprintf(user_info, sizeof(user_info), "User: %s/%s | ", term->user_name, term->user_group);
            }
        } else if (term->user_station[0]) {
            snprintf(user_info, sizeof(user_info), "Slot: %s | ", term->user_station);
        }

        if (term_w >= 145) {
            snprintf(bar, sizeof(bar), " PLATO | %s:%d | %sRet:Next(x2:Sh)  F8/ESC:Back(x2:Sh)  F4/^S:Stop(x2:Sh)  F1:Help(x2:Sh)  ^Y:Copy  ^C:Quit",
                     host, port, user_info);
        } else if (term_w >= 95) {
            snprintf(bar, sizeof(bar), " PLATO | %s:%d | %sRet(x2:Sh)  F8:Back(x2:Sh)  F4:Stop(x2:Sh)  ^Y:Copy  ^C:Quit",
                     host, port, user_info);
        } else {
            snprintf(bar, sizeof(bar), " PLATO | %s | %sRet(x2:Sh) F8:Back F4:Stop ^Y:Copy ^C:Quit",
                     host, user_info);
        }
    }

    int bar_len = (int)strlen(bar);

    char out[1024];
    int len = snprintf(out, sizeof(out), "\033[%d;1H\033[7m", status_y);
    if (len > 0) write(STDOUT_FILENO, out, len);

    write(STDOUT_FILENO, bar, bar_len);
    for (int i = bar_len; i < term_w; i++) {
        write(STDOUT_FILENO, " ", 1);
    }
    write(STDOUT_FILENO, "\033[0m", 4);
}

static size_t console_read_sequence(int fd, uint8_t *buf, size_t max_len) {
    ssize_t n = read(fd, buf, 1);
    if (n <= 0) return 0;
    size_t total = 1;

    if (buf[0] == 0x1B) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        while (total < max_len && poll(&pfd, 1, 60) > 0 && (pfd.revents & POLLIN)) {
            ssize_t m = read(fd, &buf[total], 1);
            if (m <= 0) break;
            total++;
            uint8_t last = buf[total - 1];
            if (total == 2 && last != '[' && last != 'O') {
                break;
            }
            if (total >= 3 && ((last >= '@' && last <= '~') || last == '$')) {
                break;
            }
        }
    }
    return total;
}

int plato_console_run(const char *host, int port) {
    const char *target_host = (host && host[0]) ? host : CYBER1_DEFAULT_HOST;
    int target_port = (port > 0) ? port : CYBER1_DEFAULT_PORT;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = console_signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);

    if (!console_enable_raw_mode()) {
        fprintf(stderr, "[PlatoLives] Errore: impossibile inizializzare la modalita RAW del terminale.\n");
        return 1;
    }

    plato_terminal_t term;
    plato_terminal_init(&term);
    term.beep_callback = console_beep_callback;
    term.metadata_callback = console_metadata_callback;

    plato_ringbuf_t ringbuf;
    plato_ringbuf_init(&ringbuf);

    plato_transport_t *transport = plato_transport_create();
    term.transport = transport;

    console_net_worker_ctx_t net_ctx = {
        .transport = transport,
        .ringbuf = &ringbuf,
        .running = true
    };
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, console_net_worker, &net_ctx);

    bool ok = plato_transport_connect(transport, target_host, target_port);
    if (!ok) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "\033[2J\033[10;10H\033[31mErrore di connessione a %s:%d\033[0m\r\n\033[12;10HPremere un tasto per uscire...", target_host, target_port);
        write(STDOUT_FILENO, err_msg, strlen(err_msg));
        char dummy;
        while (!g_quit && read(STDIN_FILENO, &dummy, 1) <= 0) {
            usleep(50000);
        }
        net_ctx.running = false;
        pthread_join(net_thread, NULL);
        plato_transport_destroy(transport);
        plato_ringbuf_destroy(&ringbuf);
        console_restore_terminal();
        return 1;
    }

    plato_text_cell_t prev_screen[PLATO_ROWS][PLATO_COLS];
    memset(prev_screen, 0xFF, sizeof(prev_screen));

    int term_w = 80, term_h = 34;
    int offset_x = 1, offset_y = 1;
    int status_y = 34;

    uint8_t feed_buf[4096];
    size_t feed_len = 0;
    size_t feed_pos = 0;

    char render_buf[32768];
    char tmp_cell_str[8];

    while (!g_quit) {
        double now = get_time_sec();

        if (g_pending.active && now >= g_pending.expiry) {
            flush_pending_key(&term);
        }

        if (g_resized) {
            g_resized = 0;
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
                term_w = ws.ws_col;
                term_h = ws.ws_row;
            }
            offset_x = (term_w > PLATO_COLS) ? ((term_w - PLATO_COLS) / 2 + 1) : 1;
            int avail_h = (term_h > 1) ? (term_h - 1) : term_h;
            offset_y = (avail_h > PLATO_ROWS) ? ((avail_h - PLATO_ROWS) / 2 + 1) : 1;
            status_y = term_h;

            write(STDOUT_FILENO, "\033[2J", 4);
            draw_bezel(offset_x, offset_y, term_w, term_h);
            memset(prev_screen, 0xFF, sizeof(prev_screen));
            g_status_dirty = 1;
        }

        while (1) {
            if (feed_pos >= feed_len) {
                feed_pos = 0;
                feed_len = plato_ringbuf_read(&ringbuf, feed_buf, sizeof(feed_buf));
                if (feed_len == 0) break;
            }
            term.delay_requested = false;
            size_t rem = feed_len - feed_pos;
            size_t consumed = plato_terminal_feed(&term, &feed_buf[feed_pos], rem);
            feed_pos += consumed;
            if (term.delay_requested) {
                term.delay_requested = false;
                usleep(8000);
            }
        }

        size_t r_pos = 0;
        const char *amber_prefix = "\033[38;2;255;140;0m";
        size_t amb_len = strlen(amber_prefix);
        memcpy(&render_buf[r_pos], amber_prefix, amb_len);
        r_pos += amb_len;

        bool drew_any = false;
        for (int r = 0; r < PLATO_ROWS; r++) {
            for (int c = 0; c < PLATO_COLS; c++) {
                plato_text_cell_t cell = term.text_grid[r][c];

                if (prev_screen[r][c].ch != cell.ch || prev_screen[r][c].charset != cell.charset) {
                    prev_screen[r][c] = cell;
                    drew_any = true;

                    const char *glyph_str = plato_cell_to_utf8(&term, cell, tmp_cell_str);
                    int scr_r = offset_y + r;
                    int scr_c = offset_x + c;
                    int jump_len = snprintf(&render_buf[r_pos], sizeof(render_buf) - r_pos - 32, "\033[%d;%dH%s", scr_r, scr_c, glyph_str);
                    if (jump_len > 0) r_pos += jump_len;

                    if (r_pos > sizeof(render_buf) - 512) {
                        write(STDOUT_FILENO, render_buf, r_pos);
                        r_pos = 0;
                    }
                }
            }
        }

        if (drew_any && r_pos > 0) {
            write(STDOUT_FILENO, render_buf, r_pos);
        }

        now = get_time_sec();
        if (g_status_dirty || (g_feedback_until > 0.0 && now >= g_feedback_until)) {
            if (g_feedback_until > 0.0 && now >= g_feedback_until) {
                g_feedback_until = 0.0;
            }
            g_status_dirty = 0;
            render_status_bar(&term, target_host, target_port, term_w, status_y);
        }

        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_res = poll(&pfd, 1, 16);
        if (poll_res > 0 && (pfd.revents & POLLIN)) {
            uint8_t seq[32];
            size_t seq_len = console_read_sequence(STDIN_FILENO, seq, sizeof(seq));
            if (seq_len > 0) {
                if (seq[0] == 0x1B) {
                    if (seq_len == 1) {
                        trigger_key_double_tap(&term, PLATO_KEY_BACK, false);
                    } else if (seq_len == 2) {
                        if (seq[1] == 's') {
                            trigger_key_double_tap(&term, PLATO_KEY_STOP, false);
                        } else if (seq[1] == 'S') {
                            trigger_key_double_tap(&term, PLATO_KEY_STOP, true);
                        } else if (seq[1] == 'b') {
                            trigger_key_double_tap(&term, PLATO_KEY_BACK, false);
                        } else if (seq[1] == 'B') {
                            trigger_key_double_tap(&term, PLATO_KEY_BACK, true);
                        } else if (seq[1] == 'n' || seq[1] == 'N' || seq[1] == 10 || seq[1] == 13) {
                            trigger_key_double_tap(&term, PLATO_KEY_NEXT, true);
                        } else if (seq[1] == 'c' || seq[1] == 'C') {
                            flush_pending_key(&term);
                            copy_screen_to_clipboard(&term);
                        }
                    } else if (seq[1] == 'O') {
                        switch (seq[2]) {
                            case 'P': trigger_key_double_tap(&term, PLATO_KEY_HELP, false); break;
                            case 'Q': trigger_key_double_tap(&term, PLATO_KEY_LAB, false); break;
                            case 'R': trigger_key_double_tap(&term, PLATO_KEY_DATA, false); break;
                            case 'S': trigger_key_double_tap(&term, PLATO_KEY_STOP, false); break;
                        }
                    } else if (seq[1] == '[') {
                        if (seq_len >= 3 && seq[2] == 'A') {
                            flush_pending_key(&term);
                            plato_protocol_send_key(&term, plato_keyboard_keycode(PLATO_KEY_SUPER, false));
                        } else if (seq_len >= 3 && seq[2] == 'B') {
                            flush_pending_key(&term);
                            plato_protocol_send_key(&term, plato_keyboard_keycode(PLATO_KEY_SUB, false));
                        } else if (seq_len >= 3 && seq[2] == 'C') {
                            flush_pending_key(&term);
                            uint8_t tab = 0x09; plato_transport_send(transport, &tab, 1);
                        } else if (seq_len >= 3 && seq[2] == 'D') {
                            flush_pending_key(&term);
                            plato_protocol_send_key(&term, plato_keyboard_keycode(PLATO_KEY_ERASE, false));
                        }
                        else if (seq_len >= 6 && seq[2] == '1' && seq[3] == ';' && seq[4] == '2') {
                            switch (seq[5]) {
                                case 'P': trigger_key_double_tap(&term, PLATO_KEY_HELP, true); break;
                                case 'Q': trigger_key_double_tap(&term, PLATO_KEY_LAB, true); break;
                                case 'R': trigger_key_double_tap(&term, PLATO_KEY_DATA, true); break;
                                case 'S': trigger_key_double_tap(&term, PLATO_KEY_STOP, true); break;
                            }
                        }
                        else {
                            int fnum = 0;
                            int fmod = 1;
                            size_t p = 2;
                            while (p < seq_len && seq[p] >= '0' && seq[p] <= '9') {
                                fnum = fnum * 10 + (seq[p] - '0');
                                p++;
                            }
                            if (p < seq_len && seq[p] == ';') {
                                p++;
                                fmod = 0;
                                while (p < seq_len && seq[p] >= '0' && seq[p] <= '9') {
                                    fmod = fmod * 10 + (seq[p] - '0');
                                    p++;
                                }
                            }
                            if (p < seq_len && seq[p] == '~') {
                                bool is_shifted = (fmod == 2);
                                switch (fnum) {
                                    case 15: trigger_key_double_tap(&term, PLATO_KEY_EDIT, is_shifted); break;
                                    case 17: trigger_key_double_tap(&term, PLATO_KEY_HELP, true); break;
                                    case 18: trigger_key_double_tap(&term, PLATO_KEY_LAB, true); break;
                                    case 19: trigger_key_double_tap(&term, PLATO_KEY_BACK, is_shifted); break;
                                    case 20: trigger_key_double_tap(&term, PLATO_KEY_DATA, true); break;
                                    case 21: trigger_key_double_tap(&term, PLATO_KEY_STOP, is_shifted); break;
                                }
                            }
                        }
                    }
                }
                else {
                    uint8_t ch = seq[0];
                    if (ch == 0x03) {
                        g_quit = 1;
                        break;
                    } else if (ch == 0x19) {
                        flush_pending_key(&term);
                        copy_screen_to_clipboard(&term);
                    } else if (ch == 0x0F) {
                        trigger_key_double_tap(&term, PLATO_KEY_STOP, true);
                    } else if (ch == 0x13) {
                        trigger_key_double_tap(&term, PLATO_KEY_STOP, false);
                    } else if (ch == 0x0E) {
                        trigger_key_double_tap(&term, PLATO_KEY_NEXT, false);
                    } else if (ch == 0x02) {
                        trigger_key_double_tap(&term, PLATO_KEY_BACK, false);
                    } else if (ch == 0x08) {
                        trigger_key_double_tap(&term, PLATO_KEY_HELP, false);
                    } else if (ch == 0x0C) {
                        trigger_key_double_tap(&term, PLATO_KEY_LAB, false);
                    } else if (ch == 0x04) {
                        trigger_key_double_tap(&term, PLATO_KEY_DATA, false);
                    } else if (ch == 0x05) {
                        trigger_key_double_tap(&term, PLATO_KEY_EDIT, false);
                    } else if (ch == 0x01) {
                        flush_pending_key(&term);
                        plato_protocol_send_key(&term, plato_keyboard_keycode(PLATO_KEY_ANS, false));
                    } else if (ch == 0x12) {
                        g_resized = 1;
                    } else if (ch == 10 || ch == 13) {
                        trigger_key_double_tap(&term, PLATO_KEY_NEXT, false);
                    } else if (ch == 127 || ch == 8) {
                        flush_pending_key(&term);
                        plato_protocol_send_key(&term, plato_keyboard_keycode(PLATO_KEY_ERASE, false));
                    } else if (ch == 9) {
                        flush_pending_key(&term);
                        uint8_t tab = 0x09;
                        plato_transport_send(transport, &tab, 1);
                    } else if (ch >= 32 && ch <= 126) {
                        flush_pending_key(&term);
                        plato_transport_send(transport, &ch, 1);
                    }
                }
            }
        }
    }

    console_restore_terminal();

    net_ctx.running = false;
    plato_transport_disconnect(transport);
    pthread_join(net_thread, NULL);
    plato_transport_destroy(transport);
    plato_ringbuf_destroy(&ringbuf);

    return 0;
}
