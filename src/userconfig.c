/*
 * userconfig.c — persistent per-user settings (see userconfig.h).
 *
 * The file is a flat list of KEY=value lines. Reads reuse the same trim/
 * unquote rules as the .env parser; writes rewrite the whole file through a
 * temporary file and rename(2) so a crash can never leave a half-written or
 * world-readable config behind.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "userconfig.h"
#include "buffer.h"

/* Characters treated as insignificant leading/trailing whitespace. */
static const char ORC_WS[] = " \t\r\n";

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Parse one line in place, looking for `name=value`. On a match returns a
 * malloc'd copy of the trimmed, unquoted value (never empty) or nullptr on
 * allocation failure; *matched distinguishes "our key, empty value" from
 * "not our key". `line` is mutated but not freed.
 */
static char *parse_kv_line(char *line, const char *name, bool *matched)
{
    *matched = false;

    char *p = line + strspn(line, ORC_WS);
    if (*p == '\0' || *p == '#')
        return nullptr;

    size_t nlen = strlen(name);
    if (strncmp(p, name, nlen) != 0 || p[nlen] != '=')
        return nullptr;

    *matched = true;
    p += nlen + 1;

    p += strspn(p, ORC_WS);                 /* trim leading value whitespace */
    char *end = p + strlen(p);
    while (end > p && is_ws(end[-1]))        /* ...and trailing whitespace */
        end--;

    if (end - p >= 2 && (*p == '"' || *p == '\'') && end[-1] == *p) {
        p++;                                 /* strip one matching quote pair */
        end--;
    }
    *end = '\0';

    return *p ? strdup(p) : nullptr;
}

/* Non-mutating test: is `line` a `name=...` assignment? */
static bool line_has_key(const char *line, const char *name)
{
    const char *p = line + strspn(line, ORC_WS);
    if (*p == '\0' || *p == '#')
        return false;
    size_t nlen = strlen(name);
    return strncmp(p, name, nlen) == 0 && p[nlen] == '=';
}

char *kv_read(const char *path, const char *name)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return nullptr;

    char   *line = nullptr;
    size_t  cap = 0;
    char   *value = nullptr;
    while (getline(&line, &cap, f) != -1) {
        bool matched;
        char *v = parse_kv_line(line, name, &matched);
        if (v) { value = v; break; }
        if (matched) break;                  /* key present but empty: stop */
    }
    free(line);
    fclose(f);
    return value;
}

char *uconf_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *base;
    const char *fmt;
    if (xdg && *xdg) {
        base = xdg;
        fmt  = "%s/orc/config";
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return nullptr;
        base = home;
        fmt  = "%s/.config/orc/config";
    }

    int n = snprintf(nullptr, 0, fmt, base);
    if (n < 0)
        return nullptr;
    char *path = malloc((size_t)n + 1);
    if (!path)
        return nullptr;
    snprintf(path, (size_t)n + 1, fmt, base);
    return path;
}

char *uconf_get(const char *name)
{
    char *path = uconf_path();
    if (!path)
        return nullptr;
    char *v = kv_read(path, name);
    free(path);
    return v;
}

/* Create `dir` and any missing parents, each with mode `mode`. */
static int mkdir_p(const char *dir, mode_t mode)
{
    char *tmp = strdup(dir);
    if (!tmp)
        return -1;

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) { free(tmp); return -1; }
        *p = '/';
    }
    int rc = (mkdir(tmp, mode) != 0 && errno != EEXIST) ? -1 : 0;
    free(tmp);
    return rc;
}

/* write(2) all `n` bytes, retrying short writes and EINTR. */
static int write_all(int fd, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        n   -= (size_t)w;
    }
    return 0;
}

/*
 * Rewrite the config file with `name` set to `value`, or removed when
 * `value` is nullptr. Existing lines for other keys (and comments) are kept
 * verbatim; the target key is replaced in place or appended.
 */
static int uconf_write(const char *name, const char *value)
{
    char *path = uconf_path();
    if (!path) {
        fprintf(stderr, "error: cannot locate config (set HOME or XDG_CONFIG_HOME)\n");
        return -1;
    }

    /* Ensure the parent directory exists (0700). */
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir_p(path, 0700) != 0) {
            fprintf(stderr, "error: cannot create %s: %s\n", path, strerror(errno));
            *slash = '/';
            free(path);
            return -1;
        }
        *slash = '/';
    }

    /* Build the new contents: existing lines minus the target key. */
    Buffer out = {0};
    bool   bad = false;
    FILE  *f = fopen(path, "r");
    if (f) {
        char   *line = nullptr;
        size_t  cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, f)) != -1) {
            if (line_has_key(line, name))
                continue;
            if (buf_append(&out, line, (size_t)len) != 0) { bad = true; break; }
        }
        free(line);
        fclose(f);
    }
    if (value && !bad) {
        if (out.len && out.data[out.len - 1] != '\n')
            bad |= buf_append(&out, "\n", 1) != 0;
        bad |= buf_append_str(&out, name) != 0;
        bad |= buf_append(&out, "=", 1) != 0;
        bad |= buf_append_str(&out, value) != 0;
        bad |= buf_append(&out, "\n", 1) != 0;
    }
    if (bad) {
        fprintf(stderr, "error: out of memory\n");
        buf_free(&out);
        free(path);
        return -1;
    }

    /* Write to a temp file (0600), then atomically rename into place. */
    size_t plen = strlen(path);
    char  *tmp = malloc(plen + 5);
    if (!tmp) {
        fprintf(stderr, "error: out of memory\n");
        buf_free(&out);
        free(path);
        return -1;
    }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    bool ok = true;
    int  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", tmp, strerror(errno));
        ok = false;
    } else {
        (void)fchmod(fd, 0600);              /* enforce 0600 on a reused temp */
        if (out.len && write_all(fd, out.data, out.len) != 0) ok = false;
        if (close(fd) != 0) ok = false;
        if (ok && rename(tmp, path) != 0) {
            fprintf(stderr, "error: cannot update %s: %s\n", path, strerror(errno));
            ok = false;
        }
        if (!ok)
            unlink(tmp);
    }

    free(tmp);
    buf_free(&out);
    free(path);
    return ok ? 0 : -1;
}

int uconf_set(const char *name, const char *value)   { return uconf_write(name, value); }
int uconf_unset(const char *name)                    { return uconf_write(name, nullptr); }
