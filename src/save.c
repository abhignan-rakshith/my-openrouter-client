/*
 * save.c — export a single reply to a Markdown file.
 *
 * See save.h for the contract. The path handling mirrors conv.c so the
 * saves/ directory behaves like conversations/: names may not contain a
 * slash, and a ".md" suffix is added when absent.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "save.h"

/* Longest suffix we may append when a name lacks it. */
static constexpr char MD_EXT[] = ".md";

int save_path(const char *name, char *out, size_t outsz)
{
    if (!name || !*name || strchr(name, '/'))
        return -1;

    size_t nlen = strlen(name);
    size_t elen = sizeof MD_EXT - 1;
    const char *suffix =
        (nlen > elen && strcmp(name + nlen - elen, MD_EXT) == 0)
            ? "" : MD_EXT;

    int n = snprintf(out, outsz, "%s/%s%s", ORC_SAVE_DIR, name, suffix);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

int save_ensure_dir(void)
{
    if (mkdir(ORC_SAVE_DIR, 0755) == 0 || errno == EEXIST)
        return 0;
    fprintf(stderr, "error: cannot create %s/: %s\n",
            ORC_SAVE_DIR, strerror(errno));
    return -1;
}

int save_write(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "error: cannot write %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    int rc = 0;
    size_t len = strlen(content);
    if (len && fwrite(content, 1, len, f) != len)
        rc = -1;
    /* Ensure exactly one trailing newline for a tidy file. */
    if (rc == 0 && (len == 0 || content[len - 1] != '\n') && fputc('\n', f) == EOF)
        rc = -1;
    if (rc == 0 && fflush(f) != 0)
        rc = -1;
    if (fclose(f) != 0)
        rc = -1;
    if (rc != 0)
        fprintf(stderr, "error: write failed on %s: %s\n",
                path, strerror(errno));
    return rc;
}
