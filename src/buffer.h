/*
 * buffer.h — growable byte buffer.
 *
 * A tiny dynamic string/byte accumulator used throughout the client:
 * HTTP response collection, SSE line reassembly, and JSON assembly.
 * The buffer keeps its contents NUL-terminated at all times, so
 * `data` is always safe to use as a C string.
 */
#ifndef ORC_BUFFER_H
#define ORC_BUFFER_H

#include <stddef.h>

typedef struct {
    char  *data; /* NUL-terminated contents (NULL until first append) */
    size_t len;  /* bytes stored, excluding the terminator            */
    size_t cap;  /* bytes allocated                                   */
} Buffer;

/*
 * Append n bytes from src. Returns 0 on success, -1 on failure, where
 * failure means either an allocation error or a size request that would
 * overflow SIZE_MAX. On failure the buffer contents are left intact.
 * Passing (NULL, 0) is valid and simply ensures the buffer is
 * NUL-terminated without appending anything.
 */
int buf_append(Buffer *b, const char *src, size_t n);

/* Append a NUL-terminated string. Same return convention. */
int buf_append_str(Buffer *b, const char *s);

/* Release the storage and reset the buffer to empty. */
void buf_free(Buffer *b);

#endif /* ORC_BUFFER_H */
