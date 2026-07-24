/*
 * clipboard.c — system clipboard image access.
 *
 * Pulls an image off the OS clipboard and writes it out as PNG, then
 * (portably) turns a saved PNG into the base64 data URL the OpenRouter
 * multimodal format expects.
 *
 * The capture half is platform-specific:
 *   - macOS: osascript coerces the pasteboard to «class PNGf» first,
 *     failing fast when no image is present, then writes it out.
 *   - Linux/BSD: a Wayland (wl-paste) or X11 (xclip) helper is invoked,
 *     chosen at runtime from the session environment. The helper is an
 *     external dependency, not a link-time one; when it is absent we say
 *     so distinctly rather than reporting an empty clipboard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "clipboard.h"

/* Reject a path that could break out of the single-quoted shell word.
 * Paths here are program-generated (attachments dir + timestamp), so
 * this never trips in practice; it is a guard against future callers. */
static int clip_path_is_safe(const char *path)
{
    return !strchr(path, '\'') && !strchr(path, '"') && !strchr(path, '\\');
}

#if defined(__APPLE__)

int clip_image_save(const char *path)
{
    if (!clip_path_is_safe(path))
        return -1;

    char cmd[1024];
    int n = snprintf(cmd, sizeof cmd,
        "osascript"
        " -e 'set d to the clipboard as «class PNGf»'"
        " -e 'set f to open for access POSIX file \"%s\" with write permission'"
        " -e 'set eof f to 0'"
        " -e 'write d to f'"
        " -e 'close access f'"
        " >/dev/null 2>&1", path);
    if (n < 0 || (size_t)n >= sizeof cmd)
        return -1;

    if (system(cmd) == 0) {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0)
            return 0;
    }
    remove(path);       /* don't leave an empty/partial file behind */
    return 1;
}

#else  /* Linux, BSD, and other Unix: Wayland or X11 helper. */

#include <sys/wait.h>

/*
 * The clipboard helper for the current session, reading an image/png
 * target to stdout. Wayland is preferred when a Wayland session is
 * present (an XWayland client also exposes DISPLAY, so check WAYLAND
 * first). Returns NULL when no display server is in the environment.
 */
static const char *clip_reader_cmd(void)
{
    if (getenv("WAYLAND_DISPLAY"))
        return "wl-paste --no-newline --type image/png";
    if (getenv("DISPLAY"))
        return "xclip -selection clipboard -t image/png -o";
    return NULL;
}

int clip_image_save(const char *path)
{
    if (!clip_path_is_safe(path))
        return -1;

    const char *reader = clip_reader_cmd();
    if (!reader)
        return -1;      /* no Wayland/X11 session to read a clipboard from */

    char cmd[1024];
    int n = snprintf(cmd, sizeof cmd, "%s > '%s' 2>/dev/null", reader, path);
    if (n < 0 || (size_t)n >= sizeof cmd)
        return -1;

    int rc = system(cmd);
    if (rc == 0) {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0)
            return 0;
    }
    remove(path);       /* don't leave an empty/partial file behind */

    /* Exit status 127 means the shell could not find the helper: report
     * "internal error" so the caller can advise installing it, rather
     * than the misleading "no image on the clipboard". */
    if (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 127)
        return -1;
    return 1;           /* helper ran, but the clipboard held no image */
}

#endif

char *clip_file_data_url(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return nullptr;
    }

    unsigned char *data = malloc((size_t)sz);
    if (!data || fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        return nullptr;
    }
    fclose(f);

    static const char prefix[] = "data:image/png;base64,";
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t plen = sizeof prefix - 1;
    size_t b64  = ((size_t)sz + 2) / 3 * 4;
    char *out = malloc(plen + b64 + 1);
    if (!out) {
        free(data);
        return nullptr;
    }
    memcpy(out, prefix, plen);

    char *o = out + plen;
    size_t i = 0;
    for (; i + 3 <= (size_t)sz; i += 3) {
        unsigned v = (unsigned)data[i] << 16 |
                     (unsigned)data[i + 1] << 8 |
                     (unsigned)data[i + 2];
        *o++ = tab[v >> 18];
        *o++ = tab[(v >> 12) & 0x3F];
        *o++ = tab[(v >> 6) & 0x3F];
        *o++ = tab[v & 0x3F];
    }
    if (i < (size_t)sz) {               /* 1 or 2 trailing bytes */
        unsigned v = (unsigned)data[i] << 16;
        bool two = i + 1 < (size_t)sz;
        if (two)
            v |= (unsigned)data[i + 1] << 8;
        *o++ = tab[v >> 18];
        *o++ = tab[(v >> 12) & 0x3F];
        *o++ = two ? tab[(v >> 6) & 0x3F] : '=';
        *o++ = '=';
    }
    *o = '\0';
    free(data);
    return out;
}
