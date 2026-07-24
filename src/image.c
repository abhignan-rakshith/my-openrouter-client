/*
 * image.c — save and render model-generated images. See image.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "image.h"

/* ------------------------------------------------------------------ */
/* base64 decode                                                       */
/* ------------------------------------------------------------------ */

/* Sextet value of a base64 digit, or -1 if it is not one. */
static int b64val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/*
 * Decode base64 text into a malloc'd byte buffer, storing the length in
 * *outlen. Whitespace is skipped; decoding stops at '=' padding or any
 * non-base64 byte. Returns NULL on allocation failure.
 */
static unsigned char *b64_decode(const char *s, size_t *outlen)
{
    size_t cap = strlen(s) / 4 * 3 + 4;
    unsigned char *out = malloc(cap);
    if (!out)
        return nullptr;

    size_t o = 0;
    int quad[4], qn = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '=')
            break;
        int v = b64val((unsigned char)*p);
        if (v < 0)
            continue;               /* skip whitespace / stray bytes */
        quad[qn++] = v;
        if (qn == 4) {
            out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
            out[o++] = (unsigned char)(((quad[2] & 0x3) << 6) | quad[3]);
            qn = 0;
        }
    }
    if (qn == 2) {
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
    } else if (qn == 3) {
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        out[o++] = (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
    }
    *outlen = o;
    return out;
}

/* ------------------------------------------------------------------ */
/* data: URL parsing                                                   */
/* ------------------------------------------------------------------ */

/*
 * Validate a "data:<mime>;base64,<payload>" URL. On success returns a
 * pointer to the payload (just past "base64,") and copies the mime
 * subtype (e.g. "png", "jpeg") into ext. Returns NULL otherwise.
 */
static const char *data_url_payload(const char *url, char *ext, size_t extsz)
{
    if (strncmp(url, "data:", 5) != 0)
        return nullptr;
    const char *comma = strstr(url, "base64,");
    if (!comma)
        return nullptr;

    const char *mime = url + 5;
    const char *semi = memchr(mime, ';', (size_t)(comma - mime));
    const char *mend = semi ? semi : comma;
    const char *slash = memchr(mime, '/', (size_t)(mend - mime));
    const char *sub = slash ? slash + 1 : mime;
    size_t sublen = (size_t)(mend - sub);
    if (sublen == 0 || sublen >= extsz)
        sublen = extsz - 1;         /* clamp; malformed mimes still save */
    memcpy(ext, sub, sublen);
    ext[sublen] = '\0';
    return comma + 7;               /* strlen("base64,") */
}

char *img_save_data_url(const char *data_url, const char *dir,
                        long ts, int index)
{
    char ext[16];
    const char *payload = data_url_payload(data_url, ext, sizeof ext);
    if (!payload)
        return nullptr;

    size_t len = 0;
    unsigned char *bytes = b64_decode(payload, &len);
    if (!bytes || len == 0) {
        free(bytes);
        return nullptr;
    }

    char path[512];
    int pn = snprintf(path, sizeof path, "%s/img-%ld-%d.%s",
                      dir, ts, index, ext);
    if (pn < 0 || (size_t)pn >= sizeof path) {
        free(bytes);
        return nullptr;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(bytes);
        return nullptr;
    }
    size_t w = fwrite(bytes, 1, len, f);
    free(bytes);
    if (fclose(f) != 0 || w != len) {
        remove(path);
        return nullptr;
    }
    return strdup(path);
}

/* ------------------------------------------------------------------ */
/* Inline rendering via chafa                                          */
/* ------------------------------------------------------------------ */

/* Cell box chafa fits the image into (width x height), preserving aspect.
 * A modest inline preview rather than a screen-filling render. */
#define CHAFA_SIZE "72x24"

/* True if the chafa helper is on PATH. Cached after the first check. */
static bool chafa_available(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = system("command -v chafa >/dev/null 2>&1") == 0 ? 1 : 0;
    return cached == 1;
}

bool img_render_file(const char *path)
{
    if (!isatty(STDOUT_FILENO))
        return false;
    /* The path is program-generated, but reject anything that could break
     * out of the single-quoted shell word regardless. */
    if (strpbrk(path, "'\"\\"))
        return false;
    if (!chafa_available())
        return false;

    char cmd[600];
    int n = snprintf(cmd, sizeof cmd,
                     "chafa --size=%s '%s'", CHAFA_SIZE, path);
    if (n < 0 || (size_t)n >= sizeof cmd)
        return false;

    fflush(stdout);
    return system(cmd) == 0;
}

void img_render_markers(const char *text)
{
    static const char tag[] = "[image: ";
    size_t taglen = sizeof tag - 1;
    for (const char *p = text; (p = strstr(p, tag)) != nullptr; ) {
        const char *start = p + taglen;
        const char *end = strchr(start, ']');
        if (!end)
            break;
        size_t len = (size_t)(end - start);
        char path[512];
        if (len < sizeof path) {
            memcpy(path, start, len);
            path[len] = '\0';
            img_render_file(path);
        }
        p = end + 1;
    }
}
