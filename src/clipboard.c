/*
 * clipboard.c — macOS clipboard image access.
 *
 * osascript does the pasteboard work: the AppleScript coerces the
 * clipboard to PNG data first (failing fast when no image is present,
 * before the output file is even created) and then writes it out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "clipboard.h"

int clip_image_save(const char *path)
{
    /* The path is program-generated (attachments dir + timestamp), so
     * quoting it inside the single-quoted shell word is safe; reject
     * anything that would break out regardless. */
    if (strchr(path, '\'') || strchr(path, '"') || strchr(path, '\\'))
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
