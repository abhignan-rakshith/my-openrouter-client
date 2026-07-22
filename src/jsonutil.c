/*
 * jsonutil.c — minimal hand-rolled JSON utilities.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonutil.h"

/* ------------------------------------------------------------------ */
/* Escaping                                                            */
/* ------------------------------------------------------------------ */

char *json_escape(const char *s)
{
    /* Worst case: every byte becomes a \uXXXX sequence (6 chars). */
    size_t cap = strlen(s) * 6 + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    char *o = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  *o++ = '\\'; *o++ = '"';  break;
        case '\\': *o++ = '\\'; *o++ = '\\'; break;
        case '\b': *o++ = '\\'; *o++ = 'b';  break;
        case '\f': *o++ = '\\'; *o++ = 'f';  break;
        case '\n': *o++ = '\\'; *o++ = 'n';  break;
        case '\r': *o++ = '\\'; *o++ = 'r';  break;
        case '\t': *o++ = '\\'; *o++ = 't';  break;
        default:
            /* Remaining control characters must be \u-escaped;
             * everything else (including UTF-8 bytes) passes through. */
            if (*p < 0x20)
                o += sprintf(o, "\\u%04x", *p);
            else
                *o++ = (char)*p;
        }
    }
    *o = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

char *json_decode_string(const char *p, const char **endp)
{
    if (*p != '"')
        return NULL;
    p++;
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;

#define PUT(c)                                            \
    do {                                                  \
        if (len + 1 >= cap) {                             \
            cap *= 2;                                     \
            char *tmp_ = realloc(out, cap);               \
            if (!tmp_) { free(out); return NULL; }        \
            out = tmp_;                                   \
        }                                                 \
        out[len++] = (char)(c);                           \
    } while (0)

    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  PUT('"');  p++; break;
            case '\\': PUT('\\'); p++; break;
            case '/':  PUT('/');  p++; break;
            case 'b':  PUT('\b'); p++; break;
            case 'f':  PUT('\f'); p++; break;
            case 'n':  PUT('\n'); p++; break;
            case 'r':  PUT('\r'); p++; break;
            case 't':  PUT('\t'); p++; break;
            case 'u': {
                unsigned cp;
                if (sscanf(p + 1, "%4x", &cp) != 1) {
                    free(out);
                    return NULL;
                }
                p += 5;
                /* High surrogate followed by \uDCxx low surrogate:
                 * combine into a single supplementary code point. */
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    p[0] == '\\' && p[1] == 'u') {
                    unsigned lo;
                    if (sscanf(p + 2, "%4x", &lo) == 1 &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                /* Emit cp as UTF-8. */
                if (cp < 0x80) {
                    PUT(cp);
                } else if (cp < 0x800) {
                    PUT(0xC0 | (cp >> 6));
                    PUT(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    PUT(0xE0 | (cp >> 12));
                    PUT(0x80 | ((cp >> 6) & 0x3F));
                    PUT(0x80 | (cp & 0x3F));
                } else {
                    PUT(0xF0 | (cp >> 18));
                    PUT(0x80 | ((cp >> 12) & 0x3F));
                    PUT(0x80 | ((cp >> 6) & 0x3F));
                    PUT(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: /* invalid escape */
                free(out);
                return NULL;
            }
        } else {
            PUT(*p);
            p++;
        }
    }
#undef PUT

    if (*p != '"') { /* ran off the end without a closing quote */
        free(out);
        return NULL;
    }
    if (endp)
        *endp = p + 1;
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* Key lookup                                                          */
/* ------------------------------------------------------------------ */

const char *json_find_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while (*p) {
        if (*p == '"') {
            const char *start = p + 1;
            /* Skip over the string body, honoring escape sequences,
             * so a quote inside a value cannot masquerade as a key. */
            const char *q = start;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1])
                    q++;
                q++;
            }
            if (!*q)
                return NULL;
            if ((size_t)(q - start) == klen &&
                strncmp(start, key, klen) == 0) {
                const char *v = q + 1;
                while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')
                    v++;
                if (*v == ':') {
                    v++;
                    while (*v == ' ' || *v == '\t' ||
                           *v == '\n' || *v == '\r')
                        v++;
                    return v;
                }
            }
            p = q + 1;
        } else {
            p++;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Response extractors                                                 */
/* ------------------------------------------------------------------ */

char *extract_content(const char *json)
{
    const char *choices = json_find_key(json, "choices");
    if (!choices)
        return NULL;
    const char *message = json_find_key(choices, "message");
    if (!message)
        return NULL;
    const char *content = json_find_key(message, "content");
    if (!content || *content != '"')
        return NULL;
    return json_decode_string(content, NULL);
}

char *extract_delta(const char *json)
{
    const char *choices = json_find_key(json, "choices");
    if (!choices)
        return NULL;
    const char *delta = json_find_key(choices, "delta");
    if (!delta)
        return NULL;
    const char *content = json_find_key(delta, "content");
    if (!content || *content != '"')
        return NULL;
    return json_decode_string(content, NULL);
}

char *extract_error(const char *json)
{
    const char *err = json_find_key(json, "error");
    if (!err)
        return NULL;
    const char *msg = json_find_key(err, "message");
    if (!msg || *msg != '"')
        return NULL;
    return json_decode_string(msg, NULL);
}
