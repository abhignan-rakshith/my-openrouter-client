/*
 * conv.c — JSONL conversation persistence.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "conv.h"
#include "jsonutil.h"
#include "md.h"

int conv_path(const char *name, char *out, size_t outsz)
{
    if (!name || !*name || strchr(name, '/'))
        return -1;
    size_t nlen = strlen(name);
    const char *suffix =
        (nlen > 6 && strcmp(name + nlen - 6, ".jsonl") == 0)
            ? "" : ".jsonl";
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
 * Sanity-check that a line looks like a message object before it is
 * replayed to the API: it must parse as {"role": "...", "content": "..."}.
 */
static int line_is_message(const char *line)
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

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int lineno = 0;
    int rc = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        lineno++;
        /* Trim trailing newline. */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
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
    if (ferror(f)) {
        fprintf(stderr, "error: read error on %s\n", path);
        rc = -1;
    }
    fclose(f);
    return rc;
}

/* Render one {"role":...,"content":...} object into a Buffer. */
static int render_message(Buffer *out, const char *role,
                          const char *content)
{
    char *esc = json_escape(content);
    if (!esc)
        return -1;
    int bad = buf_append_str(out, "{\"role\":\"") ||
              buf_append_str(out, role)           ||
              buf_append_str(out, "\",\"content\":\"") ||
              buf_append_str(out, esc)            ||
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
    fclose(f);
    buf_free(&msg);
    return rc;
}

int conv_items_add(Buffer *items, const char *role, const char *content)
{
    if (items->len && buf_append_str(items, ",") != 0)
        return -1;
    return render_message(items, role, content);
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

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int rc = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0)
            continue;

        const char *role_value = json_find_key(line, "role");
        const char *content_value = json_find_key(line, "content");
        char *role = role_value && *role_value == '"'
                         ? json_decode_string(role_value, NULL) : NULL;
        char *content = content_value && *content_value == '"'
                            ? json_decode_string(content_value, NULL) : NULL;
        if (role && content) {
            if (strcmp(role, "user") == 0) {
                printf("\nyou> %s\n", content);
            } else if (strcmp(role, "assistant") == 0) {
                if (markdown) {
                    if (md_color_enabled())
                        fputs("\n\033[1;38;5;141massistant>\033[0m\n",
                              stdout);
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

    if (rename(oldpath, newpath) == 0 || errno == ENOENT)
        return 0;

    fprintf(stderr, "error: cannot rename %s to %s: %s\n",
            oldpath, newpath, strerror(errno));
    return -1;
}
