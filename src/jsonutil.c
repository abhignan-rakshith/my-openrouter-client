/*
 * jsonutil.c — minimal hand-rolled JSON utilities.
 *
 * Parses untrusted network JSON, so correctness and bounds safety are
 * the priority; the implementations stay single-pass and allocation-lean.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jsonutil.h"

/* ------------------------------------------------------------------ */
/* Escaping                                                            */
/* ------------------------------------------------------------------ */

/*
 * escape_map[c] holds the second character of the two-character escape
 * for byte c (e.g. 'n' for '\n'), or 0 when c has no short escape.
 * Remaining control bytes (< 0x20) are emitted as "\u00XX"; everything
 * else, including raw UTF-8 continuation bytes, passes through verbatim.
 */
static const char escape_map[256] = {
    ['"'] = '"',  ['\\'] = '\\', ['\b'] = 'b',
    ['\f'] = 'f', ['\n'] = 'n',  ['\r'] = 'r', ['\t'] = 't',
};

char *json_escape(const char *s)
{
    constexpr char hex[] = "0123456789abcdef";

    /* Worst case per byte is a 6-char "\uXXXX"; guard the sizing math
     * against overflow before touching the allocator. */
    size_t len = strlen(s);
    if (len > (SIZE_MAX - 1) / 6)
        return nullptr;

    char *out = malloc(len * 6 + 1);
    if (!out)
        return nullptr;

    char *o = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char e = escape_map[*p];
        if (e) {
            *o++ = '\\';
            *o++ = e;
        } else if (*p < 0x20) {
            *o++ = '\\';
            *o++ = 'u';
            *o++ = '0';
            *o++ = '0';
            *o++ = hex[*p >> 4];
            *o++ = hex[*p & 0x0F];
        } else {
            *o++ = (char)*p;
        }
    }
    *o = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

/* Parse exactly four hex digits at s into *out; false on any non-hex
 * byte (including an early NUL, since it never matches). */
static bool parse_hex4(const char *restrict s, unsigned *restrict out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return false;
    }
    *out = v;
    return true;
}

char *json_decode_string(const char *p, const char **endp)
{
    if (*p != '"')
        return nullptr;
    p++;

    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out)
        return nullptr;

#define PUT(c)                                            \
    do {                                                  \
        if (len + 1 >= cap) {                             \
            if (cap > SIZE_MAX / 2) { free(out); return nullptr; } \
            cap *= 2;                                     \
            char *tmp_ = realloc(out, cap);               \
            if (!tmp_) { free(out); return nullptr; }     \
            out = tmp_;                                   \
        }                                                 \
        out[len++] = (char)(c);                           \
    } while (0)

    while (*p && *p != '"') {
        if (*p != '\\') {
            PUT(*p);
            p++;
            continue;
        }

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
            if (!parse_hex4(p + 1, &cp)) {
                free(out);
                return nullptr;
            }
            p += 5;
            /* High surrogate followed by a \uDCxx low surrogate:
             * combine into a single supplementary code point. */
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                unsigned lo;
                if (parse_hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
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
            return nullptr;
        }
    }
#undef PUT

    if (*p != '"') { /* ran off the end without a closing quote */
        free(out);
        return nullptr;
    }
    if (endp)
        *endp = p + 1;
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* Key lookup                                                          */
/* ------------------------------------------------------------------ */

static inline bool is_json_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

const char *json_find_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    for (const char *p = json; *p; ) {
        if (*p != '"') {
            p++;
            continue;
        }

        const char *start = p + 1;
        /* Skip the string body, honoring escapes, so a quote inside a
         * value cannot masquerade as a key. */
        const char *q = start;
        while (*q && *q != '"') {
            if (*q == '\\' && q[1])
                q++;
            q++;
        }
        if (!*q)
            return nullptr;

        if ((size_t)(q - start) == klen && memcmp(start, key, klen) == 0) {
            const char *v = q + 1;
            while (is_json_ws(*v))
                v++;
            if (*v == ':') {
                v++;
                while (is_json_ws(*v))
                    v++;
                return v;
            }
        }
        p = q + 1;
    }
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Response extractors                                                 */
/* ------------------------------------------------------------------ */

/* Walk a chain of nested keys, then decode the string value found at
 * the end. Returns nullptr if any hop is missing or the leaf is not a
 * JSON string. */
static char *extract_nested_string(const char *json,
                                   const char *outer, const char *inner)
{
    const char *v = json_find_key(json, outer);
    if (!v)
        return nullptr;
    v = json_find_key(v, inner);
    if (!v || *v != '"')
        return nullptr;
    return json_decode_string(v, nullptr);
}

char *extract_content(const char *json)
{
    const char *choices = json_find_key(json, "choices");
    if (!choices)
        return nullptr;
    return extract_nested_string(choices, "message", "content");
}

char *extract_delta(const char *json)
{
    const char *choices = json_find_key(json, "choices");
    if (!choices)
        return nullptr;
    return extract_nested_string(choices, "delta", "content");
}

char *extract_error(const char *json)
{
    return extract_nested_string(json, "error", "message");
}
