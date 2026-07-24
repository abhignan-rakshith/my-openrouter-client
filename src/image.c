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
/* Kitty graphics protocol                                             */
/* ------------------------------------------------------------------ */

bool img_terminal_supports_graphics(void)
{
    if (getenv("KITTY_WINDOW_ID"))
        return true;
    const char *term = getenv("TERM");
    if (term && (strstr(term, "kitty") || strstr(term, "ghostty")))
        return true;
    const char *tp = getenv("TERM_PROGRAM");
    if (tp && (strcmp(tp, "ghostty") == 0 || strcmp(tp, "WezTerm") == 0))
        return true;
    return false;
}

bool img_render_kitty(const char *data_url)
{
    char ext[16];
    const char *payload = data_url_payload(data_url, ext, sizeof ext);
    if (!payload || strcmp(ext, "png") != 0)   /* f=100 accepts PNG only */
        return false;
    if (!img_terminal_supports_graphics())
        return false;

    size_t plen = strlen(payload);
    if (plen == 0)
        return false;

    /* Direct transmit + display (a=T), PNG (f=100). The base64 payload is
     * sent in <=4096-byte chunks; m=1 marks "more follows", m=0 the last.
     * Only the first chunk carries the format keys. */
    constexpr size_t CHUNK = 4096;
    for (size_t off = 0; off < plen; off += CHUNK) {
        size_t n = plen - off < CHUNK ? plen - off : CHUNK;
        int more = (off + n < plen) ? 1 : 0;
        if (off == 0)
            printf("\033_Gf=100,a=T,m=%d;", more);
        else
            printf("\033_Gm=%d;", more);
        fwrite(payload + off, 1, n, stdout);
        fputs("\033\\", stdout);
    }
    putchar('\n');
    fflush(stdout);
    return true;
}
