/*
 * buffer.c — growable byte buffer implementation.
 */
#include <stdlib.h>
#include <string.h>

#include "buffer.h"

int buf_append(Buffer *b, const char *src, size_t n)
{
    /* Grow geometrically so repeated appends stay amortized O(1). */
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p)
            return -1;
        b->data = p;
        b->cap  = cap;
    }
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
    b->data = NULL;
    b->len = b->cap = 0;
}
