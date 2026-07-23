/*
 * lineedit.c — raw-mode interactive line editor.
 *
 * Kept deliberately small: single-line editing with horizontal
 * scrolling (linenoise-style), UTF-8 characters stepped and measured
 * with mbrtowc/wcwidth, and Ctrl+V routed to a caller-supplied hook.
 * ISIG stays enabled so Ctrl-C still delivers SIGINT; the fatal-signal
 * path restores the terminal via le_signal_restore().
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "lineedit.h"

/* ------------------------------------------------------------------ */
/* Raw mode                                                            */
/* ------------------------------------------------------------------ */

static struct termios g_orig;
static volatile sig_atomic_t g_raw = 0;

void le_signal_restore(void)
{
    if (g_raw)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
}

static int raw_enable(void)
{
    if (tcgetattr(STDIN_FILENO, &g_orig) == -1)
        return -1;
    struct termios raw = g_orig;
    /* No echo, no line buffering, no extended processing (so Ctrl+V is
     * delivered as a plain 0x16 byte, not treated as literal-next).
     * ISIG stays on: Ctrl-C must still raise SIGINT. */
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)IXON;   /* free Ctrl-S/Q for future use */
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        return -1;
    g_raw = 1;
    return 0;
}

static void raw_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_raw = 0;
}

/* ------------------------------------------------------------------ */
/* UTF-8 stepping and display width                                    */
/* ------------------------------------------------------------------ */

/* Byte length of the UTF-8 character starting at s (1 for invalid). */
static size_t ch_len(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Start offset of the character preceding byte offset i. */
static size_t ch_prev(const char *s, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    return i;
}

/* Display columns of the (at most n-byte) character at s. */
static size_t ch_width(const char *s, size_t n)
{
    wchar_t wc;
    mbstate_t st = {0};
    size_t r = mbrtowc(&wc, s, n, &st);
    if (r == (size_t)-1 || r == (size_t)-2)
        return 1;                      /* undecodable: assume one cell */
    int w = wcwidth(wc);
    return w >= 0 ? (size_t)w : 1;
}

/* Total display columns of bytes [from, to) of s. */
static size_t span_width(const char *s, size_t from, size_t to)
{
    size_t w = 0;
    for (size_t i = from; i < to; ) {
        size_t l = ch_len(s + i);
        if (i + l > to)
            break;
        w += ch_width(s + i, l);
        i += l;
    }
    return w;
}

/* Display columns of the prompt, skipping ANSI CSI escape sequences. */
static size_t prompt_width(const char *p)
{
    size_t w = 0;
    for (size_t i = 0; p[i]; ) {
        if (p[i] == '\033') {
            i++;
            if (p[i] == '[') {
                i++;
                while (p[i] && !isalpha((unsigned char)p[i]))
                    i++;
                if (p[i])
                    i++;
            }
            continue;
        }
        size_t l = ch_len(p + i);
        w += ch_width(p + i, l);
        i += l;
    }
    return w;
}

/* ------------------------------------------------------------------ */
/* Editor state and rendering                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *prompt;
    size_t      promptw;
    char       *buf;    /* NUL-terminated line being edited */
    size_t      len;
    size_t      cap;
    size_t      pos;    /* cursor, as a byte offset into buf */
    size_t      off;    /* first visible byte (horizontal scroll) */
} Le;

static size_t term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/*
 * Repaint the edit line in place: carriage return, prompt, the visible
 * window of the buffer, clear-to-eol, then park the cursor. The window
 * scrolls horizontally so the cursor always stays on screen — long
 * lines never wrap, which keeps single-line repainting correct.
 */
static void refresh(Le *le)
{
    size_t cols  = term_cols();
    size_t avail = cols > le->promptw + 1 ? cols - le->promptw - 1 : 1;

    if (le->pos < le->off)
        le->off = le->pos;
    while (span_width(le->buf, le->off, le->pos) > avail)
        le->off += ch_len(le->buf + le->off);

    /* Extend the window to whatever still fits after the offset. */
    size_t end = le->off, w = 0;
    while (end < le->len) {
        size_t l  = ch_len(le->buf + end);
        size_t cw = ch_width(le->buf + end, l);
        if (w + cw > avail)
            break;
        w   += cw;
        end += l;
    }

    fputs("\r", stdout);
    fputs(le->prompt, stdout);
    fwrite(le->buf + le->off, 1, end - le->off, stdout);
    fputs("\033[K", stdout);

    size_t cur = le->promptw + span_width(le->buf, le->off, le->pos);
    if (cur > 0)
        fprintf(stdout, "\r\033[%zuC", cur);
    else
        fputs("\r", stdout);
    fflush(stdout);
}

/* Insert n bytes at the cursor. 0 on success, -1 on OOM (line intact). */
static int insert_text(Le *le, const char *s, size_t n)
{
    if (le->len + n + 1 > le->cap) {
        size_t nc = le->cap ? le->cap : 128;
        while (nc < le->len + n + 1) {
            if (nc > SIZE_MAX / 2)
                return -1;
            nc *= 2;
        }
        char *nb = realloc(le->buf, nc);
        if (!nb)
            return -1;
        le->buf = nb;
        le->cap = nc;
    }
    memmove(le->buf + le->pos + n, le->buf + le->pos, le->len - le->pos);
    memcpy(le->buf + le->pos, s, n);
    le->len += n;
    le->pos += n;
    le->buf[le->len] = '\0';
    return 0;
}

/* Delete the character under the cursor (Delete / Ctrl-D mid-line). */
static void delete_at(Le *le)
{
    if (le->pos >= le->len)
        return;
    size_t l = ch_len(le->buf + le->pos);
    if (le->pos + l > le->len)
        l = le->len - le->pos;
    memmove(le->buf + le->pos, le->buf + le->pos + l,
            le->len - le->pos - l);
    le->len -= l;
    le->buf[le->len] = '\0';
}

/* Delete the character before the cursor (Backspace). */
static void backspace(Le *le)
{
    if (le->pos == 0)
        return;
    size_t p = ch_prev(le->buf, le->pos);
    memmove(le->buf + p, le->buf + le->pos, le->len - le->pos);
    le->len -= le->pos - p;
    le->pos  = p;
    le->buf[le->len] = '\0';
}

/* Delete the word before the cursor (Ctrl-W). */
static void kill_word(Le *le)
{
    size_t p = le->pos;
    while (p > 0 && le->buf[p - 1] == ' ')
        p--;
    while (p > 0 && le->buf[p - 1] != ' ')
        p--;
    memmove(le->buf + p, le->buf + le->pos, le->len - le->pos);
    le->len -= le->pos - p;
    le->pos  = p;
    le->buf[le->len] = '\0';
}

/* Consume an escape sequence and apply the cursor/delete keys we know. */
static void handle_escape(Le *le)
{
    char s0, s1;
    if (read(STDIN_FILENO, &s0, 1) != 1)
        return;
    if (s0 != '[' && s0 != 'O')
        return;
    if (read(STDIN_FILENO, &s1, 1) != 1)
        return;

    if (s1 >= '0' && s1 <= '9') {
        /* Extended sequence: swallow "<digits>(;<digits>)~". */
        char c = 0;
        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (!((c >= '0' && c <= '9') || c == ';'))
                break;
        }
        if (s1 == '3' && c == '~')      /* Delete key */
            delete_at(le);
        return;
    }
    switch (s1) {
    case 'C': if (le->pos < le->len)
                  le->pos += ch_len(le->buf + le->pos);
              break;                    /* right */
    case 'D': le->pos = ch_prev(le->buf, le->pos); break;   /* left */
    case 'H': le->pos = 0;        break;                    /* Home */
    case 'F': le->pos = le->len;  break;                    /* End  */
    default:  break;                    /* up/down etc.: ignored */
    }
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

/* Non-terminal stdin: plain prompt + getline, trimmed like the editor. */
static char *fallback_getline(const char *prompt)
{
    fputs(prompt, stdout);
    fflush(stdout);
    char *line = nullptr;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n == -1) {
        free(line);
        return nullptr;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    return line;
}

char *le_readline(const char *prompt, LePasteCb on_paste, void *paste_user)
{
    if (!isatty(STDIN_FILENO) || raw_enable() != 0)
        return fallback_getline(prompt);

    Le le = {
        .prompt  = prompt,
        .promptw = prompt_width(prompt),
        .buf     = malloc(128),
        .cap     = 128,
    };
    if (!le.buf) {
        raw_disable();
        return fallback_getline(prompt);
    }
    le.buf[0] = '\0';

    fputs(prompt, stdout);
    fflush(stdout);

    bool done = false, eof = false;
    while (!done) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) {
            if (r == -1 && errno == EINTR) {
                refresh(&le);           /* e.g. after a resize */
                continue;
            }
            eof = (le.len == 0);
            break;
        }

        switch ((unsigned char)c) {
        case '\r':
        case '\n':
            done = true;
            break;
        case 0x04:                      /* Ctrl-D */
            if (le.len == 0) {
                eof  = true;
                done = true;
            } else {
                delete_at(&le);
                refresh(&le);
            }
            break;
        case 0x7f:                      /* Backspace */
        case 0x08:
            backspace(&le);
            refresh(&le);
            break;
        case 0x01: le.pos = 0;      refresh(&le); break;    /* Ctrl-A */
        case 0x05: le.pos = le.len; refresh(&le); break;    /* Ctrl-E */
        case 0x0b:                      /* Ctrl-K: kill to end */
            le.len = le.pos;
            le.buf[le.len] = '\0';
            refresh(&le);
            break;
        case 0x15:                      /* Ctrl-U: kill line */
            le.len = le.pos = 0;
            le.buf[0] = '\0';
            refresh(&le);
            break;
        case 0x17:                      /* Ctrl-W */
            kill_word(&le);
            refresh(&le);
            break;
        case 0x0c:                      /* Ctrl-L: clear screen */
            fputs("\033[H\033[2J", stdout);
            refresh(&le);
            break;
        case 0x16: {                    /* Ctrl+V: clipboard image */
            if (!on_paste)
                break;
            char *msg = nullptr;
            char *ins = on_paste(paste_user, &msg);
            if (msg) {
                /* Print the status on its own line, then repaint the
                 * prompt fresh below it. */
                fputs("\r\033[K", stdout);
                fputs(msg, stdout);
                fputs("\n", stdout);
                free(msg);
            }
            if (ins) {
                insert_text(&le, ins, strlen(ins));
                free(ins);
            }
            refresh(&le);
            break;
        }
        case 0x1b:                      /* escape sequence */
            handle_escape(&le);
            refresh(&le);
            break;
        default:
            if ((unsigned char)c >= 0x20) {
                /* Pull in the continuation bytes of a UTF-8 character
                 * so it is inserted (and repainted) atomically. */
                char seq[4] = { c };
                size_t need = ch_len(seq);
                size_t got = 1;
                while (got < need &&
                       read(STDIN_FILENO, seq + got, 1) == 1)
                    got++;
                insert_text(&le, seq, got);
                refresh(&le);
            }
            break;
        }
    }

    raw_disable();
    fputs("\n", stdout);
    fflush(stdout);

    if (eof) {
        free(le.buf);
        return nullptr;
    }
    return le.buf;
}
