/*
 * buffer.c — growable byte buffer implementation.
 *
 * Storage grows geometrically (doubling) so a run of appends stays
 * amortized O(1). All capacity arithmetic is checked against SIZE_MAX
 * so a hostile or simply enormous input can never wrap the size math
 * and cause a short allocation followed by an out-of-bounds write.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"

/* First non-empty allocation. A power of two keeps the doubling loop
 * landing on power-of-two capacities, which allocators handle well. */
constexpr size_t BUF_INITIAL_CAP = 4096;

/*
 * Ensure b has room for at least `needed` total bytes (this count must
 * already include the space for the NUL terminator). Returns true on
 * success, false if allocation failed or the request cannot be
 * represented without overflow. On failure the buffer is left intact.
 */
static bool buf_reserve(Buffer *restrict b, size_t needed)
{
    if (needed <= b->cap)
        return true;

    /* Double until we cover `needed`, saturating at SIZE_MAX so the
     * shift can never wrap. Starting from at least BUF_INITIAL_CAP
     * avoids a burst of tiny reallocations for small buffers. */
    size_t cap = b->cap ? b->cap : BUF_INITIAL_CAP;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {  /* next double would overflow */
            cap = needed;          /* fall back to the exact size */
            break;
        }
        cap *= 2;
    }

    char *p = realloc(b->data, cap);
    if (!p)
        return false;

    b->data = p;
    b->cap  = cap;
    return true;
}

int buf_append(Buffer *b, const char *src, size_t n)
{
    /* Total footprint is len + n bytes of content plus one terminator.
     * Guard both additions against wraparound before trusting the sum. */
    if (n > SIZE_MAX - 1 || b->len > SIZE_MAX - 1 - n)
        return -1;
    size_t needed = b->len + n + 1;

    if (!buf_reserve(b, needed))
        return -1;

    /* memcpy requires a non-NULL src even when n == 0; skip the copy so
     * callers may legitimately pass (NULL, 0) to force a NUL-terminated
     * empty buffer. */
    if (n)
        memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int buf_append_str(Buffer *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

void buf_free(Buffer *b)
{
    free(b->data);
    b->data = nullptr;
    b->len = b->cap = 0;
}
