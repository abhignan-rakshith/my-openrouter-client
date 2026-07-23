/*
 * jsonutil.h — minimal hand-rolled JSON utilities.
 *
 * This client deliberately avoids a JSON library. These helpers cover
 * exactly what the OpenRouter chat API requires:
 *
 *   - json_escape:         C string -> JSON string literal contents
 *   - json_decode_string:  JSON string literal -> C string (UTF-8)
 *   - json_find_key:       locate `"key":` and return its value pointer
 *   - extract_*:           pull specific fields out of API responses
 *
 * The finder is a linear scanner that skips string contents correctly,
 * so quotes inside values cannot fool it. It does not track nesting
 * depth; it is suitable for the known response shapes, not arbitrary
 * document queries.
 */
#ifndef ORC_JSONUTIL_H
#define ORC_JSONUTIL_H

/*
 * Escape s for embedding inside a JSON string literal (quotes not
 * included). Returns a malloc'd string, or NULL on allocation failure.
 */
char *json_escape(const char *s);

/*
 * Decode the JSON string literal starting at p (which must point at
 * the opening '"'). Handles all standard escapes; \uXXXX sequences,
 * including surrogate pairs, are decoded to UTF-8.
 *
 * Returns a malloc'd C string or NULL on malformed input / OOM.
 * If endp is non-NULL it receives a pointer past the closing quote.
 */
char *json_decode_string(const char *p, const char **endp);

/*
 * Scan json for `"key":` and return a pointer to the first character
 * of its value, or NULL if not found.
 */
const char *json_find_key(const char *json, const char *key);

/* choices[0].message.content of a non-streaming response, or NULL. */
char *extract_content(const char *json);

/* choices[0].delta.content of a streaming chunk, or NULL. */
char *extract_delta(const char *json);

/* error.message of an error payload, or NULL. */
char *extract_error(const char *json);

/* error.metadata.error_type — OpenRouter's canonical error category
 * (e.g. "rate_limit_exceeded") — or NULL when absent. */
char *extract_error_type(const char *json);

/* error.code of an error payload (the effective HTTP status, also sent
 * in mid-stream SSE error events), or 0 when absent. */
long extract_error_code(const char *json);

#endif /* ORC_JSONUTIL_H */
