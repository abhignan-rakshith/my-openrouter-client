/*
 * conv.c — JSONL conversation persistence.
 *
 * Each conversation is a file under conversations/<name>.jsonl holding one
 * chat message object per line. See conv.h for the on-disk format and the
 * contract of each public function.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "conv.h"
#include "jsonutil.h"
#include "md.h"

/* Longest suffix we may append when a name lacks it. */
static constexpr char JSONL_EXT[] = ".jsonl";

int conv_path(const char *name, char *out, size_t outsz)
{
    if (!name || !*name || strchr(name, '/'))
        return -1;

    size_t nlen = strlen(name);
    size_t elen = sizeof JSONL_EXT - 1;
    const char *suffix =
        (nlen > elen && strcmp(name + nlen - elen, JSONL_EXT) == 0)
            ? "" : JSONL_EXT;

    int n = snprintf(out, outsz, "%s/%s%s", ORC_CONV_DIR, name, suffix);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

int conv_ensure_dir(void)
{
    if (mkdir(ORC_CONV_DIR, 0755) == 0 || errno == EEXIST)
        return 0;
    fprintf(stderr, "error: cannot create %s/: %s\n",
            ORC_CONV_DIR, strerror(errno));
    return -1;
}

/*
 * Read the next line from f into the getline-managed buffer (*line, *cap),
 * stripping any trailing CR/LF so callers see the logical content only.
 * Returns the trimmed length, or -1 at EOF or on a read error (which the
 * caller distinguishes with ferror once the loop ends).
 */
static ssize_t read_line_trimmed(FILE *f, char **line, size_t *cap)
{
    ssize_t n = getline(line, cap, f);
    if (n < 0)
        return -1;
    while (n > 0 && ((*line)[n - 1] == '\n' || (*line)[n - 1] == '\r'))
        (*line)[--n] = '\0';
    return n;
}

/*
 * Sanity-check that a line looks like a message object before it is
 * replayed to the API: it must parse as {"role": "...", "content": "..."}.
 */
static bool line_is_message(const char *line)
{
    const char *role = json_find_key(line, "role");
    const char *content = json_find_key(line, "content");
    return role && *role == '"' && content && *content == '"';
}

int conv_load(const char *path, Buffer *items)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0; /* new conversation */
        fprintf(stderr, "error: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    char *line = nullptr;
    size_t cap = 0;
    ssize_t n;
    int lineno = 0;
    int rc = 0;
    while ((n = read_line_trimmed(f, &line, &cap)) != -1) {
        lineno++;
        if (n == 0)
            continue; /* blank line */
        if (!line_is_message(line)) {
            fprintf(stderr,
                    "warning: %s:%d: not a message object, skipping\n",
                    path, lineno);
            continue;
        }
        if ((items->len && buf_append_str(items, ",") != 0) ||
            buf_append(items, line, (size_t)n) != 0) {
            fprintf(stderr, "error: out of memory\n");
            rc = -1;
            break;
        }
    }
    free(line);
    if (rc == 0 && ferror(f)) {
        fprintf(stderr, "error: read error on %s\n", path);
        rc = -1;
    }
    fclose(f);
    return rc;
}

/* Render one {"role":...,"content":...} object into a Buffer. */
static int render_message(Buffer *out, const char *role, const char *content)
{
    char *esc = json_escape(content);
    if (!esc)
        return -1;
    bool bad = buf_append_str(out, "{\"role\":\"")      ||
               buf_append_str(out, role)                ||
               buf_append_str(out, "\",\"content\":\"") ||
               buf_append_str(out, esc)                 ||
               buf_append_str(out, "\"}");
    free(esc);
    return bad ? -1 : 0;
}

int conv_append(const char *path, const char *role, const char *content)
{
    Buffer msg = {0};
    if (render_message(&msg, role, content) != 0) {
        fprintf(stderr, "error: out of memory\n");
        buf_free(&msg);
        return -1;
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "error: cannot append to %s: %s\n",
                path, strerror(errno));
        buf_free(&msg);
        return -1;
    }

    int rc = 0;
    if (fprintf(f, "%s\n", msg.data) < 0 || fflush(f) != 0) {
        fprintf(stderr, "error: write failed on %s: %s\n",
                path, strerror(errno));
        rc = -1;
    }
    if (fclose(f) != 0 && rc == 0) {
        fprintf(stderr, "error: write failed on %s: %s\n",
                path, strerror(errno));
        rc = -1;
    }
    buf_free(&msg);
    return rc;
}

int conv_items_add(Buffer *items, const char *role, const char *content)
{
    if (items->len && buf_append_str(items, ",") != 0)
        return -1;
    return render_message(items, role, content);
}

/* Print one already-decoded message in the interactive prompt's style. */
static void show_message(const char *role, const char *content, int markdown)
{
    if (strcmp(role, "user") == 0) {
        printf("\nyou> %s\n", content);
    } else if (strcmp(role, "assistant") == 0) {
        if (markdown) {
            if (md_color_enabled())
                fputs("\n\033[1;38;5;141massistant>\033[0m\n", stdout);
            else
                fputs("\nassistant>\n", stdout);
            md_render(content, md_color_enabled());
        } else {
            printf("\nassistant> %s\n", content);
        }
    } else {
        printf("\n%s> %s\n", role, content);
    }
}

int conv_show(const char *path, int markdown)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        fprintf(stderr, "error: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    char *line = nullptr;
    size_t cap = 0;
    ssize_t n;
    int rc = 0;
    while ((n = read_line_trimmed(f, &line, &cap)) != -1) {
        if (n == 0)
            continue;

        const char *role_value = json_find_key(line, "role");
        const char *content_value = json_find_key(line, "content");
        char *role = role_value && *role_value == '"'
                         ? json_decode_string(role_value, nullptr) : nullptr;
        char *content = content_value && *content_value == '"'
                            ? json_decode_string(content_value, nullptr)
                            : nullptr;
        if (role && content)
            show_message(role, content, markdown);
        free(role);
        free(content);
    }
    free(line);
    if (ferror(f)) {
        fprintf(stderr, "error: read error on %s\n", path);
        rc = -1;
    }
    fclose(f);
    return rc;
}

int conv_rename(const char *oldpath, const char *newpath)
{
    if (strcmp(oldpath, newpath) == 0)
        return 0;

    struct stat st;
    if (stat(newpath, &st) == 0) {
        fprintf(stderr, "error: conversation already exists: %s\n", newpath);
        return -1;
    }
    if (errno != ENOENT) {
        fprintf(stderr, "error: cannot check %s: %s\n",
                newpath, strerror(errno));
        return -1;
    }

    /* A not-yet-created source (no messages written) is not an error: the
     * caller can simply adopt newpath. */
    if (rename(oldpath, newpath) == 0 || errno == ENOENT)
        return 0;

    fprintf(stderr, "error: cannot rename %s to %s: %s\n",
            oldpath, newpath, strerror(errno));
    return -1;
}
