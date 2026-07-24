/*
 * models.c — interactive model picker. See models.h for the contract.
 *
 * The model list is fetched (api.c), then walked object-by-object with
 * json_array_next_object so the flat json_find_key only ever sees one
 * bounded model object at a time. The picker draws on the alternate
 * screen (screen.c) in raw mode (lineedit.c), reading keys directly.
 */
#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "api.h"
#include "buffer.h"
#include "jsonutil.h"
#include "lineedit.h"
#include "md.h"
#include "models.h"
#include "screen.h"
#include "userconfig.h"

/* Where starred model ids are persisted, comma-separated. */
#define STARS_KEY "stars"

typedef struct {
    char  *id;         /* model id, e.g. "anthropic/claude-opus-4.8" */
    char  *name;       /* human name (may be NULL)                   */
    long   context;    /* context_length in tokens                   */
    double in_price;   /* prompt price, USD per token                */
    double out_price;  /* completion price, USD per token            */
    bool   img_in;     /* accepts image input (vision)               */
    bool   img_out;    /* emits image output (orc renders text only) */
    bool   starred;    /* user favourite (persisted)                 */
} Model;

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/* True if `csv` (comma-separated ids) contains `id` as a whole token. */
static bool star_listed(const char *csv, const char *id)
{
    if (!csv)
        return false;
    size_t idlen = strlen(id);
    for (const char *p = csv; *p; ) {
        const char *comma = strchr(p, ',');
        size_t tok = comma ? (size_t)(comma - p) : strlen(p);
        if (tok == idlen && memcmp(p, id, idlen) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

/* pricing.<field> as USD per token. The API sends prices as JSON strings
 * (e.g. "0.00003"); missing/blank counts as 0. */
static double price_of(const char *obj, const char *field)
{
    const char *pr = json_find_key(obj, "pricing");
    const char *v = pr ? json_find_key(pr, field) : nullptr;
    if (!v)
        return 0;
    if (*v == '"')
        v++;
    return strtod(v, nullptr);
}

/* True if architecture.<modkey> (an array like input/output_modalities)
 * lists "image". */
static bool arch_has_image(const char *obj, const char *modkey)
{
    const char *arch = json_find_key(obj, "architecture");
    const char *im = arch ? json_find_key(arch, modkey) : nullptr;
    const char *close = im && *im == '[' ? strchr(im, ']') : nullptr;
    if (!close)
        return false;
    for (const char *p = im; p + 7 <= close; p++)
        if (strncmp(p, "\"image\"", 7) == 0)
            return true;
    return false;
}

/* qsort: starred first, then case-insensitive by id. */
static int cmp_models(const void *a, const void *b)
{
    const Model *x = a, *y = b;
    if (x->starred != y->starred)
        return y->starred - x->starred;
    return strcasecmp(x->id, y->id);
}

/*
 * Parse the models response body into a malloc'd array. Returns the count
 * (0 on parse failure / empty) and stores the array in *out (NULL if
 * none). Models without an id are skipped.
 */
static size_t parse_models(const char *body, const char *stars, Model **out)
{
    *out = nullptr;
    const char *cur = json_find_key(body, "data");
    if (!cur || *cur != '[')
        return 0;

    size_t cap = 64, n = 0;
    Model *arr = malloc(cap * sizeof *arr);
    if (!arr)
        return 0;

    char *obj;
    while ((obj = json_array_next_object(&cur)) != nullptr) {
        const char *idv = json_find_key(obj, "id");
        char *id = idv && *idv == '"' ? json_decode_string(idv, nullptr)
                                      : nullptr;
        if (!id) {
            free(obj);
            continue;
        }
        if (n == cap) {
            size_t ncap = cap * 2;
            Model *tmp = realloc(arr, ncap * sizeof *arr);
            if (!tmp) {
                free(id);
                free(obj);
                break;
            }
            arr = tmp;
            cap = ncap;
        }
        const char *nv = json_find_key(obj, "name");
        char *name = nv && *nv == '"' ? json_decode_string(nv, nullptr)
                                      : nullptr;
        const char *cv = json_find_key(obj, "context_length");
        arr[n++] = (Model){
            .id        = id,
            .name      = name,
            .context   = cv ? strtol(cv, nullptr, 10) : 0,
            .in_price  = price_of(obj, "prompt"),
            .out_price = price_of(obj, "completion"),
            .img_in    = arch_has_image(obj, "input_modalities"),
            .img_out   = arch_has_image(obj, "output_modalities"),
            .starred   = star_listed(stars, id),
        };
        free(obj);
    }

    qsort(arr, n, sizeof *arr, cmp_models);
    *out = arr;
    return n;
}

static void free_models(Model *m, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(m[i].id);
        free(m[i].name);
    }
    free(m);
}

/* Rewrite the persisted star list from the current in-memory flags. */
static void persist_stars(const Model *m, size_t n)
{
    Buffer b = {0};
    bool bad = false;
    for (size_t i = 0; i < n && !bad; i++) {
        if (!m[i].starred)
            continue;
        if (b.len)
            bad = buf_append_str(&b, ",") != 0;
        bad = bad || buf_append_str(&b, m[i].id) != 0;
    }
    if (!bad) {
        if (b.len)
            uconf_set(STARS_KEY, b.data);
        else
            uconf_unset(STARS_KEY);     /* no favourites: drop the key */
    }
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

/* Context length as a compact, rounded "8k" / "128k" / "1M". */
static const char *fmt_ctx(long c, char *buf, size_t n)
{
    if (c >= 1000000)
        snprintf(buf, n, "%ldM", (c + 500000) / 1000000);
    else if (c >= 1000)
        snprintf(buf, n, "%ldk", (c + 500) / 1000);
    else
        snprintf(buf, n, "%ld", c);
    return buf;
}

/* Per-token price as $/M tokens, trimmed: "$0", "$0.30", "$3", "$30". */
static const char *fmt_price(double per_tok, char *buf, size_t n)
{
    double perM = per_tok * 1e6;
    if (perM <= 0)
        snprintf(buf, n, "$0");
    else if (perM < 1)
        snprintf(buf, n, "$%.2f", perM);
    else if (perM < 10)
        snprintf(buf, n, "$%.1f", perM);
    else
        snprintf(buf, n, "$%.0f", perM);
    return buf;
}

/* Image capability as a short column token: which direction(s) a model
 * handles images. "out"/"both" mean it can generate images, which orc
 * shows as text only. */
static const char *img_col(const Model *m)
{
    if (m->img_in && m->img_out)
        return "both";
    if (m->img_out)
        return "out";
    if (m->img_in)
        return "in";
    return "";
}

/* Case-insensitive substring test (needle assumed lowercase). */
static bool ci_contains(const char *hay, const char *needle_lc)
{
    if (!hay || !*needle_lc)
        return true;
    size_t nl = strlen(needle_lc);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               (char)tolower((unsigned char)p[i]) == needle_lc[i])
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Terminal + keys                                                     */
/* ------------------------------------------------------------------ */

static void term_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

enum { KEY_ENTER, KEY_ESC, KEY_UP, KEY_DOWN, KEY_BACKSPACE,
       KEY_STAR, KEY_CHAR, KEY_EOF, KEY_IGNORE };

/*
 * Read one logical keypress. For a printable byte, *ch receives it and
 * KEY_CHAR is returned. A lone Esc is told apart from an arrow-key CSI
 * sequence by polling briefly for a following byte.
 */
static int read_key(char *ch)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return KEY_EOF;
    if (c == '\r' || c == '\n')
        return KEY_ENTER;
    if (c == 0x7f || c == 0x08)
        return KEY_BACKSPACE;
    if (c == '\t')
        return KEY_STAR;
    if (c == 0x1b) {
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        if (poll(&pfd, 1, 50) <= 0)
            return KEY_ESC;             /* nothing followed: lone Esc */
        unsigned char s0;
        if (read(STDIN_FILENO, &s0, 1) != 1 || (s0 != '[' && s0 != 'O'))
            return KEY_ESC;
        unsigned char s1;
        if (read(STDIN_FILENO, &s1, 1) != 1)
            return KEY_ESC;
        if (s1 == 'A')
            return KEY_UP;
        if (s1 == 'B')
            return KEY_DOWN;
        /* Drain any remaining bytes of a longer CSI (e.g. ESC[6~). */
        if (s1 >= '0' && s1 <= '9') {
            unsigned char t;
            while (poll(&pfd, 1, 5) > 0 && read(STDIN_FILENO, &t, 1) == 1)
                if (t >= '@')
                    break;
        }
        return KEY_IGNORE;
    }
    if (c >= 0x20 && c < 0x7f) {
        *ch = (char)c;
        return KEY_CHAR;
    }
    return KEY_IGNORE;
}

/* ------------------------------------------------------------------ */
/* Rendering + loop                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    Model      *m;
    size_t      n;
    int        *view;       /* indices into m that match the filter */
    size_t      nview;
    char        filter[64];
    size_t      flen;
    size_t      sel;        /* highlighted position within view      */
    size_t      top;        /* first visible view row (scroll)       */
    const char *current;    /* active model id (may be NULL)         */
    bool        color;
} Picker;

/* Rebuild the filtered view, keeping the highlight on a sensible row. */
static void refilter(Picker *p)
{
    char lc[64];
    size_t i = 0;
    for (; i < p->flen && i < sizeof lc - 1; i++)
        lc[i] = (char)tolower((unsigned char)p->filter[i]);
    lc[i] = '\0';

    p->nview = 0;
    for (size_t k = 0; k < p->n; k++)
        if (ci_contains(p->m[k].id, lc) || ci_contains(p->m[k].name, lc))
            p->view[p->nview++] = (int)k;

    if (p->sel >= p->nview)
        p->sel = p->nview ? p->nview - 1 : 0;
    p->top = 0;
}

static void draw(const Picker *p, int rows, int cols)
{
    /* Reserve: title, header, legend, footer (4 lines). */
    int body = rows - 4;
    if (body < 1)
        body = 1;
    int idw = cols - 34;        /* leave room for ctx/price/vis columns */
    if (idw < 12)
        idw = 12;
    if (idw > 60)
        idw = 60;

    /* Keep the highlighted row within the visible window. */
    size_t top = p->top;
    if (p->sel < top)
        top = p->sel;
    else if (p->sel >= top + (size_t)body)
        top = p->sel - (size_t)body + 1;

    fputs("\033[?2026h\033[2J\033[H", stdout);   /* sync + clear + home */

    /* Title with live filter and match count. */
    if (p->color)
        printf("\033[1;38;5;141mSelect a model\033[0m  "
               "\033[2m(%zu)\033[0m  filter: %s\033[7m \033[0m\n",
               p->nview, p->filter);
    else
        printf("Select a model  (%zu)  filter: %s_\n",
               p->nview, p->filter);

    /* Column header. $in/$out are prices; image is the modality column. */
    if (p->color)
        printf("\033[2m  %-*s %6s %7s %7s  %-4s\033[0m\n",
               idw, "model", "ctx", "$in", "$out", "img");
    else
        printf("  %-*s %6s %7s %7s  %-4s\n",
               idw, "model", "ctx", "$in", "$out", "img");

    for (int r = 0; r < body; r++) {
        size_t vi = top + (size_t)r;
        if (vi >= p->nview) {
            putchar('\n');
            continue;
        }
        const Model *md = &p->m[p->view[vi]];
        char ctx[32], in[16], out[16];
        fmt_ctx(md->context, ctx, sizeof ctx);
        fmt_price(md->in_price, in, sizeof in);
        fmt_price(md->out_price, out, sizeof out);

        const char *star = md->starred ? "★" : " ";
        const char *img = img_col(md);
        bool is_cur = p->current && strcmp(md->id, p->current) == 0;
        bool is_sel = vi == p->sel;

        if (p->color) {
            const char *sc = md->starred ? "\033[33m" : "";  /* yellow ★ */
            if (is_sel)
                fputs("\033[7m", stdout);                     /* reverse   */
            printf("%s%s\033[0m%s %-*.*s %6s %7s %7s  %-4s%s",
                   sc, star, is_sel ? "\033[7m" : "",
                   idw, idw, md->id, ctx, in, out, img,
                   is_cur ? " ·" : "");
            fputs("\033[0m\n", stdout);
        } else {
            printf("%s%c %-*.*s %6s %7s %7s  %-4s%s\n",
                   star, is_sel ? '>' : ' ',
                   idw, idw, md->id, ctx, in, out, img,
                   is_cur ? " (current)" : "");
        }
    }

    /* Legend for the img column, then the key hints. */
    const char *legend = "img — in: accepts images (vision) · "
                         "out: generates images (orc shows text only)";
    const char *hint = "↑↓ move · type to filter · Tab ★ · Enter select "
                       "· Esc cancel";
    if (p->color)
        printf("\033[2m%s\033[0m\n\033[2m%s\033[0m", legend, hint);
    else
        printf("%s\n%s", legend, hint);

    fputs("\033[?2026l", stdout);       /* end synchronized update */
    fflush(stdout);
}

char *models_pick(const char *api_key, const char *current,
                  bool *chosen_outputs_images)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "error: /model needs an interactive terminal\n");
        return nullptr;
    }

    fputs("Fetching models…\n", stdout);
    fflush(stdout);

    Buffer body = {0};
    if (or_models_fetch(api_key, &body) != 0)
        return nullptr;

    char *stars = uconf_get(STARS_KEY);
    Model *m = nullptr;
    size_t n = parse_models(body.data, stars, &m);
    free(stars);
    buf_free(&body);
    if (n == 0) {
        fprintf(stderr, "error: no models could be parsed\n");
        free_models(m, n);
        return nullptr;
    }

    Picker p = {
        .m = m, .n = n, .current = current, .color = md_color_enabled(),
    };
    p.view = malloc(n * sizeof *p.view);
    if (!p.view) {
        free_models(m, n);
        return nullptr;
    }
    refilter(&p);

    /* Start the highlight on the current model if it is in view. */
    if (current)
        for (size_t i = 0; i < p.nview; i++)
            if (strcmp(m[p.view[i]].id, current) == 0) {
                p.sel = i;
                break;
            }

    char *chosen = nullptr;
    bool stars_dirty = false;

    if (le_raw_on() != 0) {
        free(p.view);
        free_models(m, n);
        return nullptr;
    }
    screen_alt_enter();

    for (;;) {
        int rows, cols;
        term_size(&rows, &cols);
        p.top = (p.sel >= (size_t)(rows - 4)) ? p.sel - (size_t)(rows - 4) + 1
                                              : 0;
        draw(&p, rows, cols);

        char ch;
        int k = read_key(&ch);
        if (k == KEY_ESC || k == KEY_EOF)
            break;
        if (k == KEY_ENTER) {
            if (p.nview) {
                const Model *md = &m[p.view[p.sel]];
                chosen = strdup(md->id);
                if (chosen && chosen_outputs_images)
                    *chosen_outputs_images = md->img_out;
            }
            break;
        }
        if (k == KEY_UP) {
            if (p.sel)
                p.sel--;
        } else if (k == KEY_DOWN) {
            if (p.sel + 1 < p.nview)
                p.sel++;
        } else if (k == KEY_STAR) {
            if (p.nview) {
                Model *md = &m[p.view[p.sel]];
                md->starred = !md->starred;
                stars_dirty = true;
            }
        } else if (k == KEY_BACKSPACE) {
            if (p.flen) {
                p.filter[--p.flen] = '\0';
                refilter(&p);
            }
        } else if (k == KEY_CHAR) {
            if (p.flen < sizeof p.filter - 1) {
                p.filter[p.flen++] = ch;
                p.filter[p.flen] = '\0';
                refilter(&p);
            }
        }
    }

    screen_alt_leave();
    le_raw_off();

    if (stars_dirty)
        persist_stars(m, n);

    free(p.view);
    free_models(m, n);

    /* Returning the current model unchanged is treated as "no switch". */
    if (chosen && current && strcmp(chosen, current) == 0) {
        free(chosen);
        return nullptr;
    }
    return chosen;
}
