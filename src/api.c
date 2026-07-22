/*
 * api.c — OpenRouter chat completions transport (libcurl + SSE).
 *
 * Two request shapes share one code path:
 *   - single-shot: collect the whole body, then extract the content;
 *   - streaming (SSE): reassemble `data:` lines as they arrive, echo
 *     assistant tokens to stdout, and accumulate the full reply.
 * Every exit frees the curl handle, the header slist, and all buffers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "api.h"
#include "buffer.h"
#include "jsonutil.h"
#include "spinner.h"

/* Overall transfer deadline, in seconds. */
constexpr long ORC_TIMEOUT_SECS = 300;

/* ------------------------------------------------------------------ */
/* curl write callbacks                                                */
/* ------------------------------------------------------------------ */

/* Single-shot mode: collect the whole body into a Buffer. */
static size_t collect_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Buffer *b = ud;
    size_t n = size * nmemb;
    /* Returning a short count is how a write callback signals failure. */
    return buf_append(b, ptr, n) == 0 ? n : 0;
}

/*
 * Streaming mode state. Network chunks can split SSE lines anywhere,
 * so `line` reassembles them; `raw` keeps the full body so a non-SSE
 * error payload (e.g. a plain JSON 401) can still be diagnosed after
 * the transfer; `reply` accumulates the assistant text for the caller.
 */
typedef struct {
    Buffer     line;
    Buffer     raw;
    Buffer     reply;
    Spinner   *spinner;   /* cleared as soon as content starts arriving */
    bool       printing;  /* echo tokens to stdout as they arrive       */
    bool       got_error; /* saw an in-stream error event               */
    bool       oom;       /* allocation failure mid-stream              */
    OrStreamCb on_update; /* live-render callback, or NULL              */
    void      *on_update_user;
} StreamState;

static void stream_stop_spinner(StreamState *st)
{
    spinner_stop(st->spinner);
    st->spinner = nullptr;
}

/* Handle one complete, NUL-terminated SSE line. */
static void sse_line(StreamState *st, const char *line)
{
    /* Ignore comment lines (": OPENROUTER PROCESSING") and blanks. */
    if (strncmp(line, "data:", 5) != 0)
        return;
    const char *data = line + 5;
    while (*data == ' ')
        data++;
    if (strncmp(data, "[DONE]", 6) == 0)
        return;

    char *piece = extract_delta(data);
    if (piece) {
        if (buf_append_str(&st->reply, piece) != 0)
            st->oom = true;
        /* First visible content clears the spinner. A live-render callback
         * takes over display (fed the whole reply so far); otherwise, when
         * printing, echo just the new token. In quiet/buffered mode both
         * are off and the spinner runs until the transfer completes. */
        if (!st->oom && st->on_update) {
            stream_stop_spinner(st);
            st->on_update(st->reply.data, st->on_update_user);
        } else if (!st->oom && st->printing) {
            stream_stop_spinner(st);
            fputs(piece, stdout);
            fflush(stdout);
        }
        free(piece);
        return;
    }

    /* No delta content: either a housekeeping chunk (role, usage,
     * finish_reason) or an in-stream error event. */
    char *err = extract_error(data);
    if (err) {
        stream_stop_spinner(st);
        if (st->printing && st->reply.len)
            fputc('\n', stdout);
        fprintf(stderr, "error: %s\n", err);
        st->got_error = true;
        free(err);
    }
}

static size_t sse_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    StreamState *st = ud;
    size_t n = size * nmemb;
    if (buf_append(&st->raw, ptr, n) != 0 ||
        buf_append(&st->line, ptr, n) != 0) {
        st->oom = true;
        return 0;
    }

    /* Dispatch every complete line in the accumulator, splitting on
     * '\n' and trimming a trailing '\r'. */
    char  *start = st->line.data;
    char  *end   = st->line.data + st->line.len;
    for (char *nl; (nl = memchr(start, '\n', (size_t)(end - start))); start = nl + 1) {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r')
            nl[-1] = '\0';
        sse_line(st, start);
    }

    /* Keep the unfinished tail for the next callback. */
    size_t rest = (size_t)(end - start);
    memmove(st->line.data, start, rest);
    st->line.len = rest;
    st->line.data[rest] = '\0';

    return st->oom ? 0 : n;
}

/* ------------------------------------------------------------------ */
/* Request                                                             */
/* ------------------------------------------------------------------ */

/* Report an error payload (JSON message if parseable, raw otherwise). */
static void report_http_error(long status, const char *body)
{
    char *err = body ? extract_error(body) : nullptr;
    if (err) {
        fprintf(stderr, "error (HTTP %ld): %s\n", status, err);
        free(err);
    } else {
        fprintf(stderr, "error (HTTP %ld): unexpected response:\n%s\n",
                status, body && *body ? body : "(empty)");
    }
}

/*
 * Build the JSON request body into *out.
 * Returns 0 on success, -1 on allocation failure (out is freed).
 */
static int build_body(const OrRequest *req, Buffer *out)
{
    char *esc_model = json_escape(req->model);
    if (!esc_model)
        return -1;

    bool bad = buf_append_str(out, "{\"model\":\"") ||
               buf_append_str(out, esc_model)       ||
               buf_append_str(out, "\",")           ||
               (req->stream &&
                buf_append_str(out, "\"stream\":true,")) ||
               buf_append_str(out, "\"messages\":")      ||
               buf_append_str(out, req->messages_json)   ||
               buf_append_str(out, "}");
    free(esc_model);

    if (bad) {
        buf_free(out);
        return -1;
    }
    return 0;
}

int or_chat(const OrRequest *req, char **reply)
{
    *reply = nullptr;

    Buffer body = {0};
    if (build_body(req, &body) != 0) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "error: curl init failed\n");
        buf_free(&body);
        return -1;
    }

    char auth_header[512];
    int hn = snprintf(auth_header, sizeof auth_header,
                      "Authorization: Bearer %s", req->api_key);
    if (hn < 0 || (size_t)hn >= sizeof auth_header) {
        fprintf(stderr, "error: API key is implausibly long\n");
        curl_easy_cleanup(curl);
        buf_free(&body);
        return -1;
    }

    struct curl_slist *headers = curl_slist_append(nullptr, auth_header);
    struct curl_slist *ct = headers
        ? curl_slist_append(headers, "Content-Type: application/json")
        : nullptr;
    if (!ct) {
        fprintf(stderr, "error: out of memory\n");
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        buf_free(&body);
        return -1;
    }
    headers = ct;

    Buffer      resp = {0};
    StreamState st   = {0};
    st.on_update = req->on_update;
    st.on_update_user = req->on_update_user;
    /* The callback owns display when present, so plain echo is off then. */
    st.printing = req->stream && !req->quiet && !st.on_update;
    Spinner *spinner = req->spinner ? spinner_start("Thinking") : nullptr;
    if (req->stream)
        st.spinner = spinner;

    curl_easy_setopt(curl, CURLOPT_URL, ORC_API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ORC_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orc/0.2 (native C)");
    /* The spinner runs in a second thread; disable curl's use of signals
     * so its timeout handling stays thread-safe. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (req->stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    }

    CURLcode rc = curl_easy_perform(curl);

    /* Stop whichever spinner is still live before touching stdout/stderr. */
    if (req->stream)
        stream_stop_spinner(&st);
    else
        spinner_stop(spinner);

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    buf_free(&body);

    int result = -1;
    if (rc != CURLE_OK) {
        if (st.printing && st.reply.len)
            fputc('\n', stdout);
        fprintf(stderr, "error: request failed: %s\n",
                st.oom ? "out of memory" : curl_easy_strerror(rc));
    } else if (req->stream) {
        if (st.reply.len && !st.got_error) {
            if (st.printing)
                fputc('\n', stdout);
            *reply = st.reply.data; /* transfer ownership */
            st.reply.data = nullptr;
            result = 0;
        } else if (!st.got_error) {
            /* Nothing streamed and no SSE error: the server most likely
             * replied with a plain JSON error body. */
            report_http_error(http_status, st.raw.data);
        }
    } else {
        char *content = resp.data ? extract_content(resp.data) : nullptr;
        if (content) {
            if (!req->quiet)
                printf("%s\n", content);
            *reply = content; /* transfer ownership */
            result = 0;
        } else {
            report_http_error(http_status, resp.data);
        }
    }

    buf_free(&resp);
    buf_free(&st.line);
    buf_free(&st.raw);
    buf_free(&st.reply);
    return result;
}
