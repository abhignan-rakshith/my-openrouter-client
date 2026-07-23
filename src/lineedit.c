/*
 * lineedit.c — raw-mode interactive line editor.
 *
 * Kept deliberately small: single-line editing with horizontal
 * scrolling (linenoise-style), UTF-8 characters stepped and measured
 * with mbrtowc/wcwidth, and Ctrl+V routed to a caller-supplied hook.
 * ISIG stays enabled so Ctrl-C still delivers SIGINT; the fatal-signal
 * path restores the terminal via le_signal_restore().
 *
 * Multi-line input works shell-style: \+Enter (or Shift+Enter, where
 * the terminal encodes it distinctly) commits the current line and
 * continues on a "..." continuation line; committed lines are
 * read-only. The returned string contains the embedded newlines.
 *
 * Pastes arrive via bracketed paste mode: multi-line pastes are held
 * back behind a "[Pasted #N +K lines]" placeholder and expanded into
 * the returned string on submit.
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
    if (g_raw) {
        /* write(2) is async-signal-safe; leave bracketed paste mode. */
        ssize_t r = write(STDOUT_FILENO, "\033[?2004l", 8);
        (void)r;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    }
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
    /* Bracketed paste: the terminal wraps pasted text in ESC[200~ /
     * ESC[201~, so a paste arrives as one unit instead of keystrokes
     * (and embedded newlines don't submit the line). */
    fputs("\033[?2004h", stdout);
    fflush(stdout);
    g_raw = 1;
    return 0;
}

static void raw_disable(void)
{
    fputs("\033[?2004l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_raw = 0;
}

int le_raw_on(void)
{
    if (!isatty(STDIN_FILENO))
        return -1;
    return raw_enable();
}

void le_raw_off(void)
{
    if (g_raw)
        raw_disable();
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

/* One multi-line paste held back behind a "[Pasted #N ...]" tag. */
typedef struct {
    char  *text;    /* the pasted text (malloc'd) */
    int    tag;     /* N in the placeholder      */
    size_t lines;   /* line count shown in the placeholder */
} LePaste;

/* Most held-back pastes per prompt; later ones are inserted flattened. */
constexpr size_t LE_MAX_PASTES = 16;

typedef struct {
    const char *prompt;  /* prompt for the current visual line */
    size_t      promptw;
    char       *buf;    /* NUL-terminated line being edited */
    size_t      len;
    size_t      cap;
    size_t      pos;    /* cursor, as a byte offset into buf */
    size_t      off;    /* first visible byte (horizontal scroll) */
    size_t      start;  /* first byte of the current visual line: bytes
                           before it (earlier lines ending in '\n') are
                           committed and read-only */
    LePaste     pastes[LE_MAX_PASTES];  /* multi-line pastes held back */
    size_t      npastes;
    int         next_paste;             /* next [Pasted #N] number */
} Le;

/* Prompt shown on continuation lines of a multi-line message. */
#define LE_CONT_PROMPT "... "

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

/* Delete the character before the cursor (Backspace). Stops at the
 * start of the current visual line: committed lines are read-only. */
static void backspace(Le *le)
{
    if (le->pos <= le->start)
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
    while (p > le->start && le->buf[p - 1] == ' ')
        p--;
    while (p > le->start && le->buf[p - 1] != ' ')
        p--;
    memmove(le->buf + p, le->buf + le->pos, le->len - le->pos);
    le->len -= le->pos - p;
    le->pos  = p;
    le->buf[le->len] = '\0';
}

/*
 * Commit a newline at the cursor: the line typed so far stays on
 * screen, and editing continues on a fresh "..." continuation line
 * below it. Text after the cursor moves down with it. Committed lines
 * are read-only — the cursor cannot travel back above the newline
 * (like a shell's PS2 continuation prompt).
 */
static void newline_commit(Le *le)
{
    if (insert_text(le, "\n", 1) != 0)
        return;                         /* OOM: drop the keystroke */

    /* Repaint the finished line without the tail, then drop below it. */
    fputs("\r", stdout);
    fputs(le->prompt, stdout);
    fwrite(le->buf + le->off, 1, (le->pos - 1) - le->off, stdout);
    fputs("\033[K\r\n", stdout);

    le->start   = le->pos;
    le->off     = le->pos;
    le->prompt  = LE_CONT_PROMPT;
    le->promptw = prompt_width(LE_CONT_PROMPT);
    refresh(le);
}

/* What an escape sequence asked for beyond in-place cursor motion. */
enum { ESC_NONE, ESC_NEWLINE, ESC_PASTE };

/*
 * Append one byte to a growable paste accumulator, keeping room for a
 * NUL. Returns 0, or -1 on OOM (contents intact).
 */
static int paste_byte(char **d, size_t *len, size_t *cap, char c)
{
    if (*len + 1 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        char *nb = realloc(*d, nc);
        if (!nb)
            return -1;
        *d = nb;
        *cap = nc;
    }
    (*d)[(*len)++] = c;
    return 0;
}

/*
 * Read the body of a bracketed paste (everything up to ESC[201~),
 * normalizing CR/CRLF to '\n'. A single-line paste is inserted at the
 * cursor verbatim. A multi-line paste is held back and represented by
 * a "[Pasted #N +K lines]" placeholder, spliced back into the message
 * on submit; deleting the placeholder discards the paste. On OOM the
 * paste is consumed but dropped, so stray bytes never leak as input.
 */
static void paste_read(Le *le)
{
    static const char END[] = "\033[201~";
    constexpr size_t ENDLEN = sizeof END - 1;

    char *data = nullptr;
    size_t len = 0, cap = 0, match = 0;
    bool oom = false;
    char prev = 0, c;

    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == END[match]) {
            if (++match == ENDLEN)
                break;
            continue;
        }
        /* Mismatch: the held-back terminator prefix was real content. */
        for (size_t i = 0; i < match && !oom; i++)
            oom = paste_byte(&data, &len, &cap, END[i]) != 0;
        match = (c == END[0]) ? 1 : 0;
        if (match)
            continue;
        if (c == '\n' && prev == '\r') {    /* CRLF: already emitted \n */
            prev = c;
            continue;
        }
        prev = c;
        if (c == '\r')
            c = '\n';
        if (!oom)
            oom = paste_byte(&data, &len, &cap, c) != 0;
    }

    if (oom || len == 0) {
        free(data);
        return;
    }
    data[len] = '\0';

    size_t nl = 0;
    for (size_t i = 0; i < len; i++)
        if (data[i] == '\n')
            nl++;

    if (nl == 0) {                          /* single line: type it in */
        insert_text(le, data, len);
        free(data);
        return;
    }
    if (le->npastes >= LE_MAX_PASTES) {
        /* Out of slots: insert flattened so the display invariant
         * (no '\n' in the editable segment) holds. */
        for (size_t i = 0; i < len; i++)
            if (data[i] == '\n')
                data[i] = ' ';
        insert_text(le, data, len);
        free(data);
        return;
    }

    size_t lines = nl + (data[len - 1] != '\n' ? 1 : 0);
    char tag[48];
    snprintf(tag, sizeof tag, "[Pasted #%d +%zu lines]",
             le->next_paste, lines);
    if (insert_text(le, tag, strlen(tag)) != 0) {
        free(data);
        return;
    }
    le->pastes[le->npastes++] = (LePaste){ .text  = data,
                                           .tag   = le->next_paste,
                                           .lines = lines };
    le->next_paste++;
}

/*
 * Replace each surviving "[Pasted #N ...]" placeholder in the finished
 * line with its stored text (an edited-out placeholder discards that
 * paste). Consumes le->buf and every stored paste; returns the
 * malloc'd final line — le->buf itself when nothing needed expanding.
 */
static char *expand_pastes(Le *le)
{
    char *line = le->buf;
    for (size_t i = 0; i < le->npastes; i++) {
        LePaste *p = &le->pastes[i];
        char tag[48];
        snprintf(tag, sizeof tag, "[Pasted #%d +%zu lines]",
                 p->tag, p->lines);
        char *at = strstr(line, tag);
        if (at) {
            size_t taglen = strlen(tag), txtlen = strlen(p->text);
            size_t linelen = strlen(line);
            size_t pre = (size_t)(at - line);
            char *nb = malloc(linelen - taglen + txtlen + 1);
            if (nb) {
                memcpy(nb, line, pre);
                memcpy(nb + pre, p->text, txtlen);
                memcpy(nb + pre + txtlen, at + taglen,
                       linelen - pre - taglen + 1);
                free(line);
                line = nb;
            }   /* OOM: leave the placeholder text in place */
        }
        free(p->text);
    }
    le->npastes = 0;
    return line;
}

/* Consume an escape sequence and apply the cursor/delete keys we know.
 * Returns ESC_NEWLINE when the sequence encodes Shift+Enter (or an
 * ESC-prefixed Enter), which callers turn into a line continuation. */
static int handle_escape(Le *le)
{
    char s0, s1;
    if (read(STDIN_FILENO, &s0, 1) != 1)
        return ESC_NONE;
    if (s0 == '\r' || s0 == '\n')       /* ESC+Enter (e.g. Alt+Enter) */
        return ESC_NEWLINE;
    if (s0 != '[' && s0 != 'O')
        return ESC_NONE;
    if (read(STDIN_FILENO, &s1, 1) != 1)
        return ESC_NONE;

    if (s1 >= '0' && s1 <= '9') {
        /* Extended sequence: collect "<digits>(;<digits>)*" + final. */
        char params[16] = { s1 };
        size_t np = 1;
        char c = 0;
        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (!((c >= '0' && c <= '9') || c == ';'))
                break;
            if (np < sizeof params - 1)
                params[np++] = c;
        }
        params[np] = '\0';
        if (c == '~' && strcmp(params, "200") == 0) {
            return ESC_PASTE;           /* bracketed paste follows */
        } else if (c == '~' && strcmp(params, "3") == 0) {  /* Delete */
            delete_at(le);
        } else if ((c == 'u' && strcmp(params, "13;2") == 0) ||
                   (c == '~' && strcmp(params, "27;2;13") == 0)) {
            /* Shift+Enter, from terminals speaking the CSI-u/kitty
             * protocol ("ESC[13;2u") or xterm modifyOtherKeys
             * ("ESC[27;2;13~"). Terminals that send a plain CR for
             * Shift+Enter can't be told apart from Enter; \+Enter
             * works everywhere. */
            return ESC_NEWLINE;
        }
        return ESC_NONE;
    }
    switch (s1) {
    case 'C': if (le->pos < le->len)
                  le->pos += ch_len(le->buf + le->pos);
              break;                    /* right */
    case 'D': if (le->pos > le->start)
                  le->pos = ch_prev(le->buf, le->pos);
              break;                    /* left */
    case 'H': le->pos = le->start; break;                   /* Home */
    case 'F': le->pos = le->len;   break;                   /* End  */
    default:  break;                    /* up/down etc.: ignored */
    }
    return ESC_NONE;
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
        .prompt     = prompt,
        .promptw    = prompt_width(prompt),
        .buf        = malloc(128),
        .cap        = 128,
        .next_paste = 1,
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
            if (le.pos > le.start && le.buf[le.pos - 1] == '\\') {
                /* \+Enter: drop the backslash, continue on a new line. */
                backspace(&le);
                newline_commit(&le);
            } else {
                done = true;
            }
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
        case 0x01: le.pos = le.start; refresh(&le); break;  /* Ctrl-A */
        case 0x05: le.pos = le.len;   refresh(&le); break;  /* Ctrl-E */
        case 0x0b:                      /* Ctrl-K: kill to end */
            le.len = le.pos;
            le.buf[le.len] = '\0';
            refresh(&le);
            break;
        case 0x15:                      /* Ctrl-U: kill to line start */
            memmove(le.buf + le.start, le.buf + le.pos,
                    le.len - le.pos);
            le.len -= le.pos - le.start;
            le.pos  = le.start;
            le.buf[le.len] = '\0';
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
        case 0x1b: {                    /* escape sequence */
            int esc = handle_escape(&le);
            if (esc == ESC_NEWLINE) {
                newline_commit(&le);    /* Shift+Enter */
            } else if (esc == ESC_PASTE) {
                paste_read(&le);
                refresh(&le);
            } else {
                refresh(&le);
            }
            break;
        }
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
        for (size_t i = 0; i < le.npastes; i++)
            free(le.pastes[i].text);
        free(le.buf);
        return nullptr;
    }
    return expand_pastes(&le);
}
