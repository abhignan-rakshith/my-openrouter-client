/*
 * lineedit.c — raw-mode interactive line editor.
 *
 * Kept deliberately small: readline-style editing with horizontal
 * scrolling (linenoise-style), UTF-8 characters stepped and measured
 * with mbrtowc/wcwidth, and Ctrl+V routed to a caller-supplied hook.
 * ISIG stays enabled so Ctrl-C still delivers SIGINT; the fatal-signal
 * path restores the terminal via le_signal_restore().
 *
 * Multi-line input works shell-style: \+Enter or Shift+Enter starts a
 * "..." continuation line (the kitty keyboard protocol is requested
 * while editing, so terminals that speak it encode Shift+Enter — and
 * Ctrl/Alt combinations — distinctly as CSI-u sequences). Up/Down move
 * the cursor between the lines of the message, every line staying
 * editable; at the edges they fall through to history recall. The
 * returned string contains the embedded newlines.
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
        /* write(2) is async-signal-safe; pop the kitty keyboard
         * protocol (a no-op if never pushed) and leave bracketed
         * paste mode. */
        ssize_t r = write(STDOUT_FILENO, "\033[<u\033[?2004l", 13);
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
    const char *prompt0; /* the primary (first-line) prompt */
    size_t      promptw0;
    size_t      contw;   /* display width of LE_CONT_PROMPT */
    char       *buf;    /* NUL-terminated text being edited; '\n'
                           separates the lines of a multi-line message */
    size_t      len;
    size_t      cap;
    size_t      pos;    /* cursor, as a byte offset into buf */
    size_t      off;    /* first visible byte of the cursor's line
                           (horizontal scroll) */
    size_t      lastrow; /* screen row within the block the cursor
                            occupied after the last repaint */
    LePaste     pastes[LE_MAX_PASTES];  /* multi-line pastes held back */
    size_t      npastes;
    int         next_paste;             /* next [Pasted #N] number */
    size_t      hidx;   /* history cursor; g_nhist = "not browsing" */
    char       *hsave;  /* in-progress line stashed while browsing */
} Le;

/* ------------------------------------------------------------------ */
/* Session history                                                     */
/* ------------------------------------------------------------------ */

/* Submitted messages, oldest first, for Up/Down recall. Session-only. */
static char  **g_hist;
static size_t  g_nhist, g_histcap;

/* Record a submitted message (consecutive duplicates are skipped). */
static void hist_add(const char *line)
{
    if (!line || !*line)
        return;
    if (g_nhist && strcmp(g_hist[g_nhist - 1], line) == 0)
        return;
    if (g_nhist == g_histcap) {
        size_t nc = g_histcap ? g_histcap * 2 : 32;
        char **nh = realloc(g_hist, nc * sizeof *nh);
        if (!nh)
            return;             /* OOM: just don't record it */
        g_hist = nh;
        g_histcap = nc;
    }
    char *dup = strdup(line);
    if (dup)
        g_hist[g_nhist++] = dup;
}

/* Prompt shown on continuation lines of a multi-line message. */
#define LE_CONT_PROMPT "... "

static size_t term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* Byte offset of the start of the line containing offset i. */
static size_t line_begin(const Le *le, size_t i)
{
    while (i > 0 && le->buf[i - 1] != '\n')
        i--;
    return i;
}

/* Byte offset of the end of the line containing offset i: its '\n',
 * or the end of the buffer for the last line. */
static size_t line_end(const Le *le, size_t i)
{
    while (i < le->len && le->buf[i] != '\n')
        i++;
    return i;
}

/* Row number (0-based) of the line containing offset i. */
static size_t line_row(const Le *le, size_t i)
{
    size_t r = 0;
    for (size_t j = 0; j < i; j++)
        if (le->buf[j] == '\n')
            r++;
    return r;
}

/*
 * Repaint the whole edit block in place. Every line occupies exactly
 * one terminal row (windows are clipped, never wrapped): the cursor
 * returns to the block's top row, each line is repainted with its
 * prompt and visible window, rows a shrink left behind are erased,
 * and the cursor parks on its line at its column. The cursor's line
 * scrolls horizontally so the cursor always stays on screen; other
 * lines show their leading window.
 */
static void refresh(Le *le)
{
    size_t cols = term_cols();
    size_t crow = line_row(le, le->pos);
    size_t cbeg = line_begin(le, le->pos);
    size_t cpw  = crow ? le->contw : le->promptw0;
    size_t avail = cols > cpw + 1 ? cols - cpw - 1 : 1;

    /* Keep the window inside the cursor's line, around the cursor. */
    if (le->off < cbeg || le->off > le->pos)
        le->off = cbeg;
    while (span_width(le->buf, le->off, le->pos) > avail)
        le->off += ch_len(le->buf + le->off);

    fputs("\r", stdout);
    if (le->lastrow)
        fprintf(stdout, "\033[%zuA", le->lastrow);

    size_t bol = 0, row = 0;
    for (;; row++) {
        size_t eol  = line_end(le, bol);
        size_t pw   = row ? le->contw : le->promptw0;
        size_t lav  = cols > pw + 1 ? cols - pw - 1 : 1;
        size_t from = (row == crow) ? le->off : bol;

        /* Clip the window to whatever fits after `from`. */
        size_t end = from, w = 0;
        while (end < eol) {
            size_t l  = ch_len(le->buf + end);
            size_t cw = ch_width(le->buf + end, l);
            if (w + cw > lav)
                break;
            w   += cw;
            end += l;
        }

        fputs(row ? LE_CONT_PROMPT : le->prompt0, stdout);
        fwrite(le->buf + from, 1, end - from, stdout);
        fputs("\033[K", stdout);
        if (eol >= le->len)
            break;
        fputs("\r\n", stdout);
        bol = eol + 1;
    }
    fputs("\033[J", stdout);

    /* Park the cursor on its row and column. */
    if (row > crow)
        fprintf(stdout, "\033[%zuA", row - crow);
    size_t cur = cpw + span_width(le->buf, le->off, le->pos);
    if (cur > 0)
        fprintf(stdout, "\r\033[%zuC", cur);
    else
        fputs("\r", stdout);
    le->lastrow = crow;
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

/* Delete the character before the cursor (Backspace). Deleting a
 * '\n' joins the line with the one above it. */
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

/* Delete the word before the cursor (Ctrl-W); stays within the line. */
static void kill_word(Le *le)
{
    size_t b = line_begin(le, le->pos);
    size_t p = le->pos;
    while (p > b && le->buf[p - 1] == ' ')
        p--;
    while (p > b && le->buf[p - 1] != ' ')
        p--;
    memmove(le->buf + p, le->buf + le->pos, le->len - le->pos);
    le->len -= le->pos - p;
    le->pos  = p;
    le->buf[le->len] = '\0';
}

/*
 * Insert a newline at the cursor: editing continues on a "..."
 * continuation line below, any text after the cursor moving down with
 * it. Bound to \+Enter, and to Shift+Enter where the terminal encodes
 * it distinctly.
 */
static void insert_newline(Le *le)
{
    if (insert_text(le, "\n", 1) != 0)
        return;                         /* OOM: drop the keystroke */
    refresh(le);
}

/*
 * Replace the entire edit buffer with `text` and repaint: the old
 * block is erased (cursor to its top row, clear downward) and the new
 * text painted in its place. Used by history recall.
 */
static void set_buffer(Le *le, const char *text)
{
    fputs("\r", stdout);
    if (le->lastrow)
        fprintf(stdout, "\033[%zuA", le->lastrow);
    fputs("\033[J", stdout);

    le->len = le->pos = le->off = le->lastrow = 0;
    le->buf[0] = '\0';
    insert_text(le, text, strlen(text));
    refresh(le);
}

/* Recall the previous (dir < 0) or next (dir > 0) history entry. The
 * in-progress line is stashed on first Up and restored past the end. */
static void hist_recall(Le *le, int dir)
{
    if (dir < 0) {
        if (le->hidx == 0)
            return;
        if (le->hidx == g_nhist) {      /* leaving the live line */
            free(le->hsave);
            le->hsave = strdup(le->buf);
        }
        le->hidx--;
        set_buffer(le, g_hist[le->hidx]);
    } else {
        if (le->hidx >= g_nhist)
            return;
        le->hidx++;
        set_buffer(le, le->hidx == g_nhist
                       ? (le->hsave ? le->hsave : "")
                       : g_hist[le->hidx]);
    }
}

/* Move the cursor to the line above (dir < 0) or below (dir > 0),
 * keeping its display column where possible. */
static void vertical_move(Le *le, int dir)
{
    size_t cbeg = line_begin(le, le->pos);
    size_t goal = span_width(le->buf, cbeg, le->pos);

    size_t tbeg, tend;
    if (dir < 0) {
        if (cbeg == 0)
            return;
        tbeg = line_begin(le, cbeg - 1);
        tend = cbeg - 1;                /* the '\n' ending that line */
    } else {
        size_t cend = line_end(le, le->pos);
        if (cend >= le->len)
            return;
        tbeg = cend + 1;
        tend = line_end(le, tbeg);
    }

    /* Walk the target line until `goal` display columns are covered. */
    size_t i = tbeg, w = 0;
    while (i < tend) {
        size_t l  = ch_len(le->buf + i);
        size_t cw = ch_width(le->buf + i, l);
        if (w + cw > goal)
            break;
        w += cw;
        i += l;
    }
    le->pos = i;
}

/* What an escape sequence asked for beyond in-place cursor motion.
 * Values above ESC_CTRL encode a Ctrl+letter received in CSI-u form:
 * ESC_CTRL + n stands for the C0 byte n (Ctrl+A..Ctrl+Z = 1..26). */
enum { ESC_NONE, ESC_NEWLINE, ESC_PASTE, ESC_CTRL };

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
        } else if (c == 'u') {
            /* CSI-u key ("<codepoint>;<modifiers>u"), from the kitty
             * keyboard protocol we push at le_readline entry (or a
             * terminal volunteering it). modifiers-1 is a bitmask:
             * 1=Shift, 2=Alt, 4=Ctrl. Terminals that send a plain CR
             * for Shift+Enter can't be told apart from Enter; \+Enter
             * works everywhere. */
            char *sep = strchr(params, ';');
            unsigned long cp   = strtoul(params, nullptr, 10);
            unsigned long mods = sep ? strtoul(sep + 1, nullptr, 10) : 1;
            unsigned long bits = mods ? mods - 1 : 0;
            if (cp == 13 && (bits & 3))
                return ESC_NEWLINE;     /* Shift+Enter / Alt+Enter */
            if ((bits & 4) && cp >= 'a' && cp <= 'z')
                return ESC_CTRL + (int)(cp - 'a' + 1);
            /* The Esc key itself ("27u") and the rest: ignore. */
        } else if (c == '~' && strcmp(params, "27;2;13") == 0) {
            /* Shift+Enter via xterm modifyOtherKeys. */
            return ESC_NEWLINE;
        }
        return ESC_NONE;
    }
    switch (s1) {
    case 'A':                                               /* Up   */
        if (line_begin(le, le->pos) == 0)
            hist_recall(le, -1);        /* first line: history */
        else
            vertical_move(le, -1);
        break;
    case 'B':                                               /* Down */
        if (line_end(le, le->pos) >= le->len)
            hist_recall(le, +1);        /* last line: history */
        else
            vertical_move(le, +1);
        break;
    case 'C': if (le->pos < le->len)
                  le->pos += ch_len(le->buf + le->pos);
              break;                    /* right */
    case 'D': if (le->pos > 0)
                  le->pos = ch_prev(le->buf, le->pos);
              break;                    /* left */
    case 'H': le->pos = line_begin(le, le->pos); break;     /* Home */
    case 'F': le->pos = line_end(le, le->pos);   break;     /* End  */
    default:  break;
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
        .prompt0    = prompt,
        .promptw0   = prompt_width(prompt),
        .contw      = prompt_width(LE_CONT_PROMPT),
        .buf        = malloc(128),
        .cap        = 128,
        .next_paste = 1,
        .hidx       = g_nhist,
    };
    if (!le.buf) {
        raw_disable();
        return fallback_getline(prompt);
    }
    le.buf[0] = '\0';

    /* Push the kitty keyboard protocol's disambiguate flag so
     * Shift+Enter (and Ctrl/Alt combinations) arrive as CSI-u
     * sequences. Terminals that don't speak it ignore the request;
     * popped below and by le_signal_restore. */
    fputs("\033[>1u", stdout);
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

key:
        switch ((unsigned char)c) {
        case '\r':
        case '\n':
            if (le.pos > 0 && le.buf[le.pos - 1] == '\\') {
                /* \+Enter: drop the backslash, continue on a new line. */
                backspace(&le);
                insert_newline(&le);
            } else {
                done = true;
            }
            break;
        case 0x03:                      /* Ctrl-C, in CSI-u form */
            /* Under the kitty protocol the terminal sends no C0 byte,
             * so ISIG never fires: deliver the SIGINT ourselves to
             * match the legacy behaviour. */
            raise(SIGINT);
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
        case 0x01:                      /* Ctrl-A: line start */
            le.pos = line_begin(&le, le.pos);
            refresh(&le);
            break;
        case 0x05:                      /* Ctrl-E: line end */
            le.pos = line_end(&le, le.pos);
            refresh(&le);
            break;
        case 0x0b: {                    /* Ctrl-K: kill to line end */
            size_t e = line_end(&le, le.pos);
            memmove(le.buf + le.pos, le.buf + e, le.len - e);
            le.len -= e - le.pos;
            le.buf[le.len] = '\0';
            refresh(&le);
            break;
        }
        case 0x15: {                    /* Ctrl-U: kill to line start */
            size_t b = line_begin(&le, le.pos);
            memmove(le.buf + b, le.buf + le.pos, le.len - le.pos);
            le.len -= le.pos - b;
            le.pos  = b;
            le.buf[le.len] = '\0';
            refresh(&le);
            break;
        }
        case 0x17:                      /* Ctrl-W */
            kill_word(&le);
            refresh(&le);
            break;
        case 0x0c:                      /* Ctrl-L: clear screen */
            fputs("\033[H\033[2J", stdout);
            le.lastrow = 0;             /* block repaints from the top */
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
                insert_newline(&le);    /* Shift+Enter */
            } else if (esc == ESC_PASTE) {
                paste_read(&le);
                refresh(&le);
            } else if (esc > ESC_CTRL) {
                /* A Ctrl key in CSI-u form: re-enter the dispatch
                 * with its C0 byte. */
                c = (char)(esc - ESC_CTRL);
                goto key;
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

    /* Drop below the block so later output lands after it, then pop
     * the keyboard protocol. */
    size_t lastline = line_row(&le, le.len);
    if (lastline > le.lastrow)
        fprintf(stdout, "\033[%zuB", lastline - le.lastrow);
    fputs("\033[<u", stdout);
    raw_disable();
    fputs("\n", stdout);
    fflush(stdout);

    free(le.hsave);
    if (eof) {
        for (size_t i = 0; i < le.npastes; i++)
            free(le.pastes[i].text);
        free(le.buf);
        return nullptr;
    }
    char *out = expand_pastes(&le);
    hist_add(out);
    return out;
}
