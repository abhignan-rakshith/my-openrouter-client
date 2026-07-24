/*
 * api.h — OpenRouter chat completions transport.
 *
 * Wraps libcurl for the POST to /api/v1/chat/completions, in both
 * streaming (SSE) and single-shot modes. In streaming mode content is
 * printed to stdout as it arrives; in both modes the complete
 * assistant reply is returned to the caller for persistence.
 */
#ifndef ORC_API_H
#define ORC_API_H

#include "buffer.h"

#define ORC_API_URL "https://openrouter.ai/api/v1/chat/completions"
#define ORC_API_MODELS_URL "https://openrouter.ai/api/v1/models"

/*
 * Streaming progress callback. Invoked during an SSE stream each time new
 * assistant content arrives, with the full reply accumulated so far (a
 * NUL-terminated string owned by the transport — do not free or retain
 * it). Lets the caller render the reply live; when set it takes over
 * display, so or_chat prints nothing itself.
 */
typedef void (*OrStreamCb)(const char *reply, void *user);

typedef struct {
    const char *api_key;       /* bearer token                        */
    const char *model;         /* model id                            */
    const char *messages_json; /* complete JSON array of messages     */
    int         stream;        /* 1 = SSE streaming, 0 = single-shot  */
    int         quiet;         /* 1 = don't print (caller will)       */
    int         spinner;       /* 1 = terminal spinner while waiting  */
    int         markdown;      /* 1 = caller renders completed reply  */
    OrStreamCb  on_update;     /* live-render callback, or NULL       */
    void       *on_update_user;/* opaque arg passed to on_update      */
    int         esc_cancel;    /* 1 = a bare Esc on stdin cancels the
                                  stream; any partial reply is still
                                  returned as the result. The caller
                                  must have stdin in raw mode.        */
    Buffer     *errs;          /* when set, error diagnostics are
                                  appended here instead of stderr (for
                                  callers whose screen is transient,
                                  e.g. the alternate-screen stream) */
} OrRequest;

/*
 * Perform a chat completion.
 *
 * On success returns 0 and stores the malloc'd assistant reply in
 * *reply (never NULL on success). On failure returns -1; a diagnostic
 * has already been written to stderr and *reply is NULL.
 *
 * Unless req->quiet is set, the reply is printed to stdout — token by
 * token when streaming, in one piece otherwise — with a trailing
 * newline.
 */
int or_chat(const OrRequest *req, char **reply);

/*
 * Preflight: ask OpenRouter whether `model` accepts image input, by
 * checking the model's advertised input modalities.
 *
 * Returns 1 if images are supported, 0 if not, -1 when the answer
 * could not be determined (network error, unknown model). Performs a
 * blocking HTTP GET; callers should cache the result per session.
 */
int or_model_supports_images(const char *api_key, const char *model);

/*
 * Fetch the list of models that produce text output (the only kind orc
 * can render), as the raw JSON response body, into *out. The server-side
 * output_modalities=text filter keeps image/audio/embedding-only models
 * out of the result. Returns 0 on success (*out holds the body), -1 on
 * error (a diagnostic is printed; *out is left empty).
 */
int or_models_fetch(const char *api_key, Buffer *out);

#endif /* ORC_API_H */
