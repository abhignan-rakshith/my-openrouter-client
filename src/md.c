/*
 * md.c — libcmark-backed ANSI terminal renderer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include <cmark.h>

#include "md.h"
#include "buffer.h"

enum {
    STYLE_BOLD    = 1u << 0,
    STYLE_ITALIC  = 1u << 1,
    STYLE_DIM     = 1u << 2,
    STYLE_HEADING = 1u << 3,
    STYLE_CODE    = 1u << 4,
    STYLE_LINK    = 1u << 5,
    STYLE_QUOTE   = 1u << 6,
    STYLE_CODE_BLOCK = 1u << 7,
    STYLE_STRIKE  = 1u << 8,
    STYLE_HIGHLIGHT = 1u << 9,
    STYLE_UNDERLINE = 1u << 10,
    STYLE_ACCENT  = 1u << 11,
    STYLE_SUCCESS = 1u << 12,
    STYLE_WARNING = 1u << 13
};

typedef struct {
    int color;
    int line_start;
    int wrote_any;
    int trailing_newlines;
    int indent;
    int quote_depth;
    int heading_level;
    unsigned style;
    Buffer *out;   /* when non-NULL, write here instead of stdout */
} Renderer;

static void out_bytes(const Renderer *r, const char *b, size_t n)
{
    if (r->out)
        buf_append(r->out, b, n);
    else
        fwrite(b, 1, n, stdout);
}

static void out_str(const Renderer *r, const char *s)
{
    if (r->out)
        buf_append_str(r->out, s);
    else
        fputs(s, stdout);
}

int md_stdout_is_tty(void)
{
    const char *term = getenv("TERM");
    return isatty(STDOUT_FILENO) && (!term || strcmp(term, "dumb") != 0);
}

int md_color_enabled(void)
{
    return md_stdout_is_tty() && getenv("NO_COLOR") == NULL;
}

static void emit_style(const Renderer *r)
{
    if (!r->color)
        return;

    out_str(r, "\033[0m");
    if (r->style & STYLE_HEADING) {
        /* Per-level palette so the hierarchy is visible even though a
         * terminal can't change the font size. */
        switch (r->heading_level) {
        case 1: out_str(r, "\033[1;4;38;5;199m"); break; /* pink   */
        case 2: out_str(r, "\033[1;4;38;5;45m");  break; /* cyan   */
        case 3: out_str(r, "\033[1;38;5;39m");    break; /* blue   */
        case 4: out_str(r, "\033[1;38;5;78m");    break; /* green  */
        case 5: out_str(r, "\033[1;38;5;214m");   break; /* orange */
        default: out_str(r, "\033[1;2;38;5;250m"); break;/* grey   */
        }
    } else if (r->style & STYLE_CODE_BLOCK)
        out_str(r, "\033[38;5;114m");    /* soft green */
    else if (r->style & STYLE_CODE)
        out_str(r, "\033[38;5;220m");    /* warm yellow */
    else if (r->style & STYLE_LINK)
        out_str(r, "\033[4;38;5;75m");   /* underlined blue */
    else if (r->style & STYLE_SUCCESS)
        out_str(r, "\033[1;38;5;84m");   /* green */
    else if (r->style & STYLE_WARNING)
        out_str(r, "\033[1;38;5;220m");  /* yellow */
    else if (r->style & STYLE_ACCENT)
        out_str(r, "\033[1;38;5;45m");   /* cyan */
    else if (r->style & STYLE_QUOTE)
        out_str(r, "\033[38;5;109m");    /* muted cyan */

    if (r->style & STYLE_HIGHLIGHT)
        out_str(r, "\033[48;5;229m\033[38;5;16m"); /* black on cream */
    if ((r->style & STYLE_BOLD) && !(r->style & STYLE_HEADING))
        out_str(r, "\033[1m");
    if (r->style & STYLE_ITALIC)
        out_str(r, "\033[3m");
    if (r->style & STYLE_UNDERLINE)
        out_str(r, "\033[4m");
    if (r->style & STYLE_STRIKE)
        out_str(r, "\033[9m");
    if (r->style & STYLE_DIM)
        out_str(r, "\033[2m");
}

static void set_style(Renderer *r, unsigned style)
{
    r->style = style;
    emit_style(r);
}

static void start_line(Renderer *r)
{
    if (!r->line_start)
        return;

    unsigned saved = r->style;
    if (r->quote_depth) {
        set_style(r, STYLE_QUOTE | STYLE_DIM);
        for (int i = 0; i < r->quote_depth; i++)
            out_str(r, "│ ");
        set_style(r, saved);
    }
    for (int i = 0; i < r->indent; i++)
        out_bytes(r, " ", 1);
    r->line_start = 0;
}

static void put_char(Renderer *r, char c)
{
    if (c == '\n') {
        out_bytes(r, "\n", 1);
        r->line_start = 1;
        r->wrote_any = 1;
        r->trailing_newlines++;
        return;
    }
    start_line(r);
    out_bytes(r, &c, 1);
    r->wrote_any = 1;
    r->trailing_newlines = 0;
}

static void put_text(Renderer *r, const char *text)
{
    if (!text)
        return;
    while (*text)
        put_char(r, *text++);
}

static void ensure_lines(Renderer *r, int count)
{
    if (!r->wrote_any)
        return;
    while (r->trailing_newlines < count)
        put_char(r, '\n');
}

/* ------------------------------------------------------------------ */
/* Unicode sub/superscript                                            */
/* ------------------------------------------------------------------ */

static const char *superscript_of(char c)
{
    switch (c) {
    case '0': return "⁰"; case '1': return "¹"; case '2': return "²";
    case '3': return "³"; case '4': return "⁴"; case '5': return "⁵";
    case '6': return "⁶"; case '7': return "⁷"; case '8': return "⁸";
    case '9': return "⁹"; case '+': return "⁺"; case '-': return "⁻";
    case '=': return "⁼"; case '(': return "⁽"; case ')': return "⁾";
    case 'a': return "ᵃ"; case 'b': return "ᵇ"; case 'c': return "ᶜ";
    case 'd': return "ᵈ"; case 'e': return "ᵉ"; case 'f': return "ᶠ";
    case 'g': return "ᵍ"; case 'h': return "ʰ"; case 'i': return "ⁱ";
    case 'j': return "ʲ"; case 'k': return "ᵏ"; case 'l': return "ˡ";
    case 'm': return "ᵐ"; case 'n': return "ⁿ"; case 'o': return "ᵒ";
    case 'p': return "ᵖ"; case 'r': return "ʳ"; case 's': return "ˢ";
    case 't': return "ᵗ"; case 'u': return "ᵘ"; case 'v': return "ᵛ";
    case 'w': return "ʷ"; case 'x': return "ˣ"; case 'y': return "ʸ";
    case 'z': return "ᶻ"; case '.': return "·"; case ' ': return " ";
    default:  return NULL;
    }
}

static const char *subscript_of(char c)
{
    switch (c) {
    case '0': return "₀"; case '1': return "₁"; case '2': return "₂";
    case '3': return "₃"; case '4': return "₄"; case '5': return "₅";
    case '6': return "₆"; case '7': return "₇"; case '8': return "₈";
    case '9': return "₉"; case '+': return "₊"; case '-': return "₋";
    case '=': return "₌"; case '(': return "₍"; case ')': return "₎";
    case 'a': return "ₐ"; case 'e': return "ₑ"; case 'h': return "ₕ";
    case 'i': return "ᵢ"; case 'j': return "ⱼ"; case 'k': return "ₖ";
    case 'l': return "ₗ"; case 'm': return "ₘ"; case 'n': return "ₙ";
    case 'o': return "ₒ"; case 'p': return "ₚ"; case 'r': return "ᵣ";
    case 's': return "ₛ"; case 't': return "ₜ"; case 'u': return "ᵤ";
    case 'v': return "ᵥ"; case 'x': return "ₓ"; case ' ': return " ";
    default:  return NULL;
    }
}

/* Convert ASCII `s` to sub/superscript. Unmapped bytes fall back to a
 * `^(...)`/`_(...)` wrapper so nothing is silently dropped. */
static char *to_script(const char *s, int super)
{
    Buffer out = {0};
    int mappable = 1;
    for (const char *p = s; *p; p++) {
        const char *g = super ? superscript_of(*p) : subscript_of(*p);
        if (!g) {
            mappable = 0;
            break;
        }
        buf_append_str(&out, g);
    }
    if (mappable)
        return out.data ? out.data : strdup("");
    buf_free(&out);

    Buffer fb = {0};
    buf_append_str(&fb, super ? "^(" : "_(");
    buf_append_str(&fb, s);
    buf_append_str(&fb, ")");
    return fb.data;
}

/* ------------------------------------------------------------------ */
/* Rich inline text (strikethrough, marks, scripts, autolinks)         */
/* ------------------------------------------------------------------ */

static void put_text_rich(Renderer *r, const char *s);

static void span(Renderer *r, unsigned extra, const char *start, size_t len)
{
    unsigned saved = r->style;
    set_style(r, saved | extra);
    char *tmp = strndup(start, len);
    if (tmp) {
        put_text_rich(r, tmp);
        free(tmp);
    }
    set_style(r, saved);
}

static const char *find_double(const char *p, char c)
{
    for (; p[0] && p[1]; p++)
        if (p[0] == c && p[1] == c)
            return p;
    return NULL;
}

static size_t autolink_len(const char *p)
{
    size_t n = 0;
    if (strncmp(p, "http://", 7) == 0)
        n = 7;
    else if (strncmp(p, "https://", 8) == 0)
        n = 8;
    else if (strncmp(p, "www.", 4) == 0)
        n = 4;
    else
        return 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' &&
           p[n] != '<' && p[n] != '>' && p[n] != ')' && p[n] != '"')
        n++;
    /* Trim trailing sentence punctuation. */
    while (n > 0 && strchr(".,;:!?", p[n - 1]))
        n--;
    return n;
}

static void put_scripted(Renderer *r, const char *inner, int super)
{
    char *sc = to_script(inner, super);
    put_text(r, sc);
    free(sc);
}

static void put_text_rich(Renderer *r, const char *s)
{
    if (!s)
        return;

    for (size_t i = 0; s[i];) {
        char c = s[i];

        if (c == '~' && s[i + 1] == '~') {    /* ~~strikethrough~~ */
            const char *close = find_double(s + i + 2, '~');
            if (close) {
                span(r, STYLE_STRIKE, s + i + 2,
                     (size_t)(close - (s + i + 2)));
                i = (size_t)(close - s) + 2;
                continue;
            }
        }
        if (c == '=' && s[i + 1] == '=') {    /* ==highlight== */
            const char *close = find_double(s + i + 2, '=');
            if (close) {
                span(r, STYLE_HIGHLIGHT, s + i + 2,
                     (size_t)(close - (s + i + 2)));
                i = (size_t)(close - s) + 2;
                continue;
            }
        }
        if (c == '~') {                       /* ~subscript~ */
            const char *close = strchr(s + i + 1, '~');
            if (close && close > s + i + 1 && *(close + 1) != '~') {
                char *inner = strndup(s + i + 1,
                                      (size_t)(close - (s + i + 1)));
                if (inner) { put_scripted(r, inner, 0); free(inner); }
                i = (size_t)(close - s) + 1;
                continue;
            }
        }
        if (c == '^') {                       /* ^superscript^ */
            const char *close = strchr(s + i + 1, '^');
            if (close && close > s + i + 1) {
                char *inner = strndup(s + i + 1,
                                      (size_t)(close - (s + i + 1)));
                if (inner) { put_scripted(r, inner, 1); free(inner); }
                i = (size_t)(close - s) + 1;
                continue;
            }
        }
        size_t link = autolink_len(s + i);
        if (link) {
            /* Terminal span: do not re-scan (the URL is itself a link). */
            char *url = strndup(s + i, link);
            if (url) {
                unsigned saved = r->style;
                set_style(r, saved | STYLE_LINK);
                put_text(r, url);
                set_style(r, saved);
                free(url);
            }
            i += link;
            continue;
        }

        put_char(r, c);
        i++;
    }
}

static void render_inlines(Renderer *r, cmark_node *parent);
static void render_block(Renderer *r, cmark_node *node,
                         int indent, int compact);
static size_t utf8_width(const char *text);

static void styled_children(Renderer *r, cmark_node *node,
                            unsigned extra)
{
    unsigned saved = r->style;
    set_style(r, saved | extra);
    render_inlines(r, node);
    set_style(r, saved);
}

static int link_text_is_url(cmark_node *node, const char *url)
{
    cmark_node *child = cmark_node_first_child(node);
    return child && !cmark_node_next(child) &&
           cmark_node_get_type(child) == CMARK_NODE_TEXT &&
           cmark_node_get_literal(child) &&
           strcmp(cmark_node_get_literal(child), url) == 0;
}

static void render_inline(Renderer *r, cmark_node *node)
{
    const char *literal;
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_TEXT:
        put_text_rich(r, cmark_node_get_literal(node));
        break;
    case CMARK_NODE_SOFTBREAK:
    case CMARK_NODE_LINEBREAK:
        put_char(r, '\n');
        break;
    case CMARK_NODE_CODE:
        /* CODE is a leaf, so render its literal directly. */
        if ((literal = cmark_node_get_literal(node)) != NULL) {
            unsigned saved = r->style;
            set_style(r, saved | STYLE_CODE);
            put_text(r, literal);
            set_style(r, saved);
        }
        break;
    case CMARK_NODE_EMPH:
        styled_children(r, node, STYLE_ITALIC);
        break;
    case CMARK_NODE_STRONG:
        styled_children(r, node, STYLE_BOLD);
        break;
    case CMARK_NODE_LINK: {
        styled_children(r, node, STYLE_LINK);
        const char *url = cmark_node_get_url(node);
        const char *title = cmark_node_get_title(node);
        if (url && *url && (!link_text_is_url(node, url) ||
                            (title && *title))) {
            unsigned saved = r->style;
            set_style(r, STYLE_DIM);   /* muted annotation, not underlined */
            put_text(r, " (");
            if (!link_text_is_url(node, url))
                put_text(r, url);
            if (title && *title) {
                if (!link_text_is_url(node, url))
                    put_text(r, " — ");
                put_text(r, "“");
                put_text(r, title);
                put_text(r, "”");
            }
            put_text(r, ")");
            set_style(r, saved);
        }
        break;
    }
    case CMARK_NODE_IMAGE: {
        unsigned saved = r->style;
        /* An image is not a hyperlink: render it muted, not underlined. */
        set_style(r, STYLE_DIM);
        put_text(r, "🖼 ");
        render_inlines(r, node);
        const char *url = cmark_node_get_url(node);
        const char *title = cmark_node_get_title(node);
        if (title && *title) {
            put_text(r, " — “");
            put_text(r, title);
            put_text(r, "”");
        }
        if (url && *url) {
            put_text(r, " (");
            put_text(r, url);
            put_text(r, ")");
        }
        set_style(r, saved);
        break;
    }
    case CMARK_NODE_HTML_INLINE: {
        unsigned saved = r->style;
        set_style(r, saved | STYLE_DIM);
        put_text(r, cmark_node_get_literal(node));
        set_style(r, saved);
        break;
    }
    default:
        render_inlines(r, node);
        break;
    }
}

static void render_inlines(Renderer *r, cmark_node *parent)
{
    for (cmark_node *node = cmark_node_first_child(parent);
         node; node = cmark_node_next(node))
        render_inline(r, node);
}

static void render_inlines_skip(Renderer *r, cmark_node *parent,
                                size_t first_text_bytes)
{
    int first = 1;
    for (cmark_node *node = cmark_node_first_child(parent);
         node; node = cmark_node_next(node)) {
        if (first && first_text_bytes &&
            cmark_node_get_type(node) == CMARK_NODE_TEXT) {
            const char *literal = cmark_node_get_literal(node);
            put_text_rich(r, literal ? literal + first_text_bytes : "");
        } else {
            render_inline(r, node);
        }
        first = 0;
    }
}

static void render_code_block(Renderer *r, cmark_node *node, int indent)
{
    const char *info = cmark_node_get_fence_info(node);
    const char *code = cmark_node_get_literal(node);
    unsigned saved = r->style;

    ensure_lines(r, 1);
    r->indent = indent;
    set_style(r, STYLE_DIM | STYLE_QUOTE);
    put_text(r, "┌─");
    if (info && *info) {
        put_char(r, ' ');
        put_text(r, info);
    }
    put_char(r, '\n');

    set_style(r, STYLE_CODE_BLOCK);
    const char *p = code ? code : "";
    while (*p) {
        r->indent = indent;
        put_text(r, "│ ");
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        fwrite(p, 1, len, stdout);
        r->wrote_any = 1;
        r->trailing_newlines = 0;
        put_char(r, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }

    r->indent = indent;
    set_style(r, STYLE_DIM | STYLE_QUOTE);
    put_text(r, "└─");
    put_char(r, '\n');
    set_style(r, saved);
    ensure_lines(r, 2);
}

static void render_item(Renderer *r, cmark_node *item, int indent,
                        cmark_list_type type, int number)
{
    cmark_node *child = cmark_node_first_child(item);
    int first = 1;

    for (; child; child = cmark_node_next(child)) {
        cmark_node_type child_type = cmark_node_get_type(child);
        if (first && child_type == CMARK_NODE_PARAGRAPH) {
            char marker[32];
            size_t skip = 0;
            unsigned marker_style = STYLE_ACCENT;
            cmark_node *first_inline = cmark_node_first_child(child);
            const char *literal =
                first_inline &&
                cmark_node_get_type(first_inline) == CMARK_NODE_TEXT
                    ? cmark_node_get_literal(first_inline) : NULL;
            if (literal && strlen(literal) >= 4 && literal[0] == '[' &&
                (literal[1] == ' ' || literal[1] == 'x' ||
                 literal[1] == 'X') &&
                literal[2] == ']' &&
                (literal[3] == ' ' || literal[3] == '\t')) {
                int checked = literal[1] == 'x' || literal[1] == 'X';
                snprintf(marker, sizeof marker, "%s ", checked ? "☑" : "☐");
                marker_style = checked ? STYLE_SUCCESS : STYLE_WARNING;
                skip = 4;
                while (literal[skip] == ' ' || literal[skip] == '\t')
                    skip++;
            } else if (type == CMARK_ORDERED_LIST) {
                snprintf(marker, sizeof marker, "%d. ", number);
            } else {
                snprintf(marker, sizeof marker, "• ");
            }

            r->indent = indent;
            unsigned saved = r->style;
            set_style(r, marker_style);
            put_text(r, marker);
            set_style(r, saved);
            r->indent = indent + (int)utf8_width(marker);
            render_inlines_skip(r, child, skip);
            put_char(r, '\n');
        } else if (child_type == CMARK_NODE_LIST) {
            render_block(r, child, indent + 2, 1);
        } else {
            render_block(r, child, indent + 2, 1);
        }
        first = 0;
    }
    ensure_lines(r, 1);
}

static void render_list(Renderer *r, cmark_node *node,
                        int indent, int compact)
{
    cmark_list_type type = cmark_node_get_list_type(node);
    int number = cmark_node_get_list_start(node);
    if (number < 1)
        number = 1;

    ensure_lines(r, 1);
    for (cmark_node *item = cmark_node_first_child(node);
         item; item = cmark_node_next(item), number++)
        render_item(r, item, indent, type, number);
    ensure_lines(r, compact ? 1 : 2);
}

static void render_block(Renderer *r, cmark_node *node,
                         int indent, int compact)
{
    switch (cmark_node_get_type(node)) {
    case CMARK_NODE_PARAGRAPH:
        r->indent = indent;
        render_inlines(r, node);
        put_char(r, '\n');
        ensure_lines(r, compact ? 1 : 2);
        break;

    case CMARK_NODE_HEADING: {
        ensure_lines(r, r->wrote_any ? 2 : 0);
        r->indent = indent;
        unsigned saved = r->style;
        int saved_level = r->heading_level;
        int level = cmark_node_get_heading_level(node);
        r->heading_level = level;
        /* Dim `#` prefix conveys the level; the colored title supplies
         * the visual weight. */
        set_style(r, STYLE_DIM);
        for (int i = 0; i < level; i++)
            put_char(r, '#');
        put_char(r, ' ');
        set_style(r, STYLE_HEADING);
        render_inlines(r, node);
        set_style(r, saved);
        r->heading_level = saved_level;
        put_char(r, '\n');
        ensure_lines(r, 2);
        break;
    }

    case CMARK_NODE_CODE_BLOCK:
        render_code_block(r, node, indent);
        break;

    case CMARK_NODE_BLOCK_QUOTE:
        ensure_lines(r, 1);
        r->quote_depth++;
        for (cmark_node *child = cmark_node_first_child(node);
             child; child = cmark_node_next(child))
            render_block(r, child, indent, 1);
        r->quote_depth--;
        ensure_lines(r, compact ? 1 : 2);
        break;

    case CMARK_NODE_LIST:
        render_list(r, node, indent, compact);
        break;

    case CMARK_NODE_THEMATIC_BREAK: {
        ensure_lines(r, 1);
        r->indent = indent;
        unsigned saved = r->style;
        set_style(r, STYLE_DIM | STYLE_QUOTE);
        put_text(r, "────────────────────────────────────────────────");
        set_style(r, saved);
        put_char(r, '\n');
        ensure_lines(r, compact ? 1 : 2);
        break;
    }

    case CMARK_NODE_HTML_BLOCK: {
        r->indent = indent;
        unsigned saved = r->style;
        set_style(r, STYLE_DIM);
        put_text(r, cmark_node_get_literal(node));
        set_style(r, saved);
        ensure_lines(r, compact ? 1 : 2);
        break;
    }

    default:
        for (cmark_node *child = cmark_node_first_child(node);
             child; child = cmark_node_next(child))
            render_block(r, child, indent, compact);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* GFM-style tables                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char **cells;
    size_t count;
} TableRow;

typedef struct {
    TableRow *rows; /* header is rows[0]; delimiter is not retained */
    size_t count;
    size_t cap;
    size_t columns;
    int *align;     /* 0 = left, 1 = center, 2 = right */
} Table;

static void row_free(TableRow *row)
{
    for (size_t i = 0; i < row->count; i++)
        free(row->cells[i]);
    free(row->cells);
    row->cells = NULL;
    row->count = 0;
}

static void table_free(Table *table)
{
    for (size_t i = 0; i < table->count; i++)
        row_free(&table->rows[i]);
    free(table->rows);
    free(table->align);
    memset(table, 0, sizeof *table);
}

static int row_add_cell(TableRow *row, const char *start, size_t len)
{
    while (len && (*start == ' ' || *start == '\t')) {
        start++;
        len--;
    }
    while (len && (start[len - 1] == ' ' || start[len - 1] == '\t'))
        len--;

    char *cell = malloc(len + 1);
    if (!cell)
        return -1;
    memcpy(cell, start, len);
    cell[len] = '\0';

    char **cells = realloc(row->cells, (row->count + 1) * sizeof *cells);
    if (!cells) {
        free(cell);
        return -1;
    }
    row->cells = cells;
    row->cells[row->count++] = cell;
    return 0;
}

/*
 * Split a pipe row, honoring escaped pipes and simple code spans. The
 * caller supplies a line without its trailing newline.
 */
static int parse_pipe_row(const char *line, size_t len, TableRow *row)
{
    memset(row, 0, sizeof *row);
    while (len && (line[len - 1] == '\r' ||
                   line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    while (len && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }
    if (!len)
        return 0;

    int had_pipe = 0;
    if (*line == '|') {
        line++;
        len--;
        had_pipe = 1;
    }

    /* Ignore one optional trailing pipe. */
    if (len && line[len - 1] == '|') {
        size_t slashes = 0;
        while (len > slashes + 1 && line[len - slashes - 2] == '\\')
            slashes++;
        if ((slashes & 1u) == 0) {
            len--;
            had_pipe = 1;
        }
    }

    const char *cell = line;
    size_t cell_len = 0;
    int in_code = 0;
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (c == '`') {
            size_t slashes = 0;
            while (i > slashes && line[i - slashes - 1] == '\\')
                slashes++;
            if ((slashes & 1u) == 0)
                in_code = !in_code;
        }
        if (c == '|' && !in_code) {
            size_t slashes = 0;
            while (i > slashes && line[i - slashes - 1] == '\\')
                slashes++;
            if ((slashes & 1u) == 0) {
                if (row_add_cell(row, cell, cell_len) != 0)
                    goto oom;
                cell = line + i + 1;
                cell_len = 0;
                had_pipe = 1;
                continue;
            }
        }
        cell_len++;
    }
    if (row_add_cell(row, cell, cell_len) != 0)
        goto oom;

    if (!had_pipe) {
        row_free(row);
        return 0;
    }
    return 1;

oom:
    row_free(row);
    return -1;
}

static int parse_delimiter(const TableRow *row, int **align_out)
{
    if (!row->count)
        return 0;
    int *align = calloc(row->count, sizeof *align);
    if (!align)
        return -1;

    for (size_t i = 0; i < row->count; i++) {
        const char *p = row->cells[i];
        size_t len = strlen(p);
        int left = len && p[0] == ':';
        if (left) {
            p++;
            len--;
        }
        int right = len && p[len - 1] == ':';
        if (right)
            len--;
        if (len < 1) {
            free(align);
            return 0;
        }
        for (size_t j = 0; j < len; j++) {
            if (p[j] != '-') {
                free(align);
                return 0;
            }
        }
        align[i] = left && right ? 1 : right ? 2 : 0;
    }
    *align_out = align;
    return 1;
}

static int table_add_row(Table *table, TableRow *row)
{
    if (table->count == table->cap) {
        size_t cap = table->cap ? table->cap * 2 : 8;
        TableRow *rows = realloc(table->rows, cap * sizeof *rows);
        if (!rows)
            return -1;
        table->rows = rows;
        table->cap = cap;
    }
    table->rows[table->count++] = *row;
    memset(row, 0, sizeof *row);
    return 0;
}

static size_t utf8_width(const char *text)
{
    mbstate_t state;
    memset(&state, 0, sizeof state);
    size_t width = 0;
    const char *p = text;

    while (*p) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &state);
        if (n == (size_t)-1 || n == (size_t)-2) {
            /* Malformed/incomplete UTF-8: consume one byte visibly and
             * reset the conversion state rather than losing content. */
            memset(&state, 0, sizeof state);
            p++;
            width++;
            continue;
        }
        if (n == 0)
            break;
        int cells = wcwidth(wc);
        if (cells > 0)
            width += (size_t)cells;
        p += n;
    }
    return width;
}

static void render_cell(Renderer *r, const char *cell)
{
    cmark_node *doc =
        cmark_parse_document(cell, strlen(cell), CMARK_OPT_DEFAULT);
    if (!doc) {
        put_text(r, cell);
        return;
    }
    cmark_node *block = cmark_node_first_child(doc);
    if (block && cmark_node_get_type(block) == CMARK_NODE_PARAGRAPH)
        render_inlines(r, block);
    else
        put_text(r, cell);
    cmark_node_free(doc);
}

static size_t cell_width(const char *cell)
{
    Buffer rendered = {0};
    Renderer measure = {
        .line_start = 0,
        .out = &rendered
    };
    render_cell(&measure, cell);
    size_t width = utf8_width(rendered.data ? rendered.data : cell);
    buf_free(&rendered);
    return width;
}

static void table_border(Renderer *r, const size_t *widths,
                         size_t columns, const char *left,
                         const char *middle, const char *right)
{
    unsigned saved = r->style;
    set_style(r, STYLE_DIM | STYLE_QUOTE);
    put_text(r, left);
    for (size_t col = 0; col < columns; col++) {
        for (size_t i = 0; i < widths[col] + 2; i++)
            put_text(r, "─");
        put_text(r, col + 1 == columns ? right : middle);
    }
    put_char(r, '\n');
    set_style(r, saved);
}

static void render_table(Renderer *r, const Table *table)
{
    size_t *widths = calloc(table->columns, sizeof *widths);
    if (!widths)
        return;

    for (size_t row = 0; row < table->count; row++) {
        for (size_t col = 0; col < table->columns; col++) {
            const char *cell = col < table->rows[row].count
                                   ? table->rows[row].cells[col] : "";
            size_t width = cell_width(cell);
            if (width > widths[col])
                widths[col] = width;
        }
    }
    for (size_t col = 0; col < table->columns; col++)
        if (widths[col] < 3)
            widths[col] = 3;

    ensure_lines(r, 1);
    r->indent = 0;
    table_border(r, widths, table->columns, "┌", "┬", "┐");

    for (size_t row = 0; row < table->count; row++) {
        unsigned saved = r->style;
        set_style(r, STYLE_DIM | STYLE_QUOTE);
        put_text(r, "│");

        for (size_t col = 0; col < table->columns; col++) {
            const char *cell = col < table->rows[row].count
                                   ? table->rows[row].cells[col] : "";
            size_t used = cell_width(cell);
            size_t extra = widths[col] > used ? widths[col] - used : 0;
            size_t before = 0, after = extra;
            if (table->align[col] == 2) {
                before = extra;
                after = 0;
            } else if (table->align[col] == 1) {
                before = extra / 2;
                after = extra - before;
            }

            put_char(r, ' ');
            for (size_t i = 0; i < before; i++)
                put_char(r, ' ');
            set_style(r, row == 0 ? STYLE_HEADING : saved);
            render_cell(r, cell);
            set_style(r, STYLE_DIM | STYLE_QUOTE);
            for (size_t i = 0; i < after; i++)
                put_char(r, ' ');
            put_text(r, " │");
        }
        set_style(r, saved);
        put_char(r, '\n');

        if (row == 0)
            table_border(r, widths, table->columns, "├", "┼", "┤");
    }
    table_border(r, widths, table->columns, "└", "┴", "┘");
    ensure_lines(r, 2);
    free(widths);
}

static const char *line_end(const char *p)
{
    const char *nl = strchr(p, '\n');
    return nl ? nl : p + strlen(p);
}

static const char *next_line(const char *end)
{
    return *end == '\n' ? end + 1 : end;
}

/* Detect top-level fenced code so pipe examples inside code stay code. */
static int fence_run(const char *line, size_t len, char *marker)
{
    size_t spaces = 0;
    while (spaces < len && spaces < 4 && line[spaces] == ' ')
        spaces++;
    if (spaces > 3 || spaces == len)
        return 0;
    char c = line[spaces];
    if (c != '`' && c != '~')
        return 0;
    size_t run = 0;
    while (spaces + run < len && line[spaces + run] == c)
        run++;
    if (run < 3)
        return 0;
    *marker = c;
    return (int)run;
}

static void render_commonmark(Renderer *r, const char *text, size_t len)
{
    if (!len)
        return;
    cmark_node *doc = cmark_parse_document(text, len, CMARK_OPT_DEFAULT);
    if (!doc) {
        char *copy = malloc(len + 1);
        if (!copy)
            return;
        memcpy(copy, text, len);
        copy[len] = '\0';
        put_text(r, copy);
        free(copy);
        return;
    }
    for (cmark_node *node = cmark_node_first_child(doc);
         node; node = cmark_node_next(node))
        render_block(r, node, 0, 0);
    cmark_node_free(doc);
}

void md_render(const char *text, int color)
{
    if (!text)
        text = "";

    Renderer r = {
        .color = color,
        .line_start = 1
    };
    const char *p = text;
    const char *prose = text;
    char fence_marker_char = '\0';
    int fence_length = 0;

    while (*p) {
        const char *end = line_end(p);
        size_t len = (size_t)(end - p);
        const char *following = next_line(end);

        if (!fence_length && *following) {
            const char *delim_end = line_end(following);
            TableRow header = {0}, delimiter = {0};
            int header_ok = parse_pipe_row(p, len, &header);
            int delim_ok =
                parse_pipe_row(following,
                               (size_t)(delim_end - following), &delimiter);
            int *align = NULL;
            int valid_delim = delim_ok > 0
                                  ? parse_delimiter(&delimiter, &align) : 0;

            if (header_ok > 0 && valid_delim > 0 &&
                header.count == delimiter.count) {
                Table table = {
                    .columns = header.count,
                    .align = align
                };
                row_free(&delimiter);
                if (table_add_row(&table, &header) != 0) {
                    row_free(&header);
                    table_free(&table);
                    break;
                }

                const char *after = next_line(delim_end);
                while (*after) {
                    const char *data_end = line_end(after);
                    TableRow data = {0};
                    int data_ok =
                        parse_pipe_row(after, (size_t)(data_end - after),
                                       &data);
                    if (data_ok <= 0) {
                        row_free(&data);
                        break;
                    }
                    if (table_add_row(&table, &data) != 0) {
                        row_free(&data);
                        break;
                    }
                    after = next_line(data_end);
                }

                render_commonmark(&r, prose, (size_t)(p - prose));
                render_table(&r, &table);
                table_free(&table);
                p = after;
                prose = after;
                continue;
            }
            free(align);
            row_free(&header);
            row_free(&delimiter);
        }

        char marker = '\0';
        int run = fence_run(p, len, &marker);
        if (run) {
            if (!fence_length) {
                fence_marker_char = marker;
                fence_length = run;
            } else if (marker == fence_marker_char && run >= fence_length) {
                fence_marker_char = '\0';
                fence_length = 0;
            }
        }
        p = following;
    }

    render_commonmark(&r, prose, strlen(prose));
    ensure_lines(&r, 1);
    if (r.color)
        fputs("\033[0m", stdout);
    fflush(stdout);
}
