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

#define ORC_API_URL "https://openrouter.ai/api/v1/chat/completions"

typedef struct {
    const char *api_key;       /* bearer token                        */
    const char *model;         /* model id                            */
    const char *messages_json; /* complete JSON array of messages     */
    int         stream;        /* 1 = SSE streaming, 0 = single-shot  */
    int         quiet;         /* 1 = don't print (caller will)       */
    int         spinner;       /* 1 = terminal spinner while waiting  */
    int         markdown;      /* 1 = caller renders completed reply  */
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

#endif /* ORC_API_H */
