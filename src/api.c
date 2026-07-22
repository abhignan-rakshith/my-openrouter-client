/*
 * api.c — OpenRouter chat completions transport (libcurl + SSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "api.h"
#include "buffer.h"
#include "jsonutil.h"
#include "spinner.h"

/* ------------------------------------------------------------------ */
/* curl write callbacks                                                */
/* ------------------------------------------------------------------ */

/* Single-shot mode: collect the whole body into a Buffer. */
static size_t collect_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Buffer *b = ud;
    size_t n = size * nmemb;
    if (buf_append(b, ptr, n) != 0)
        return 0; /* returning short signals failure to curl */
    return n;
}

/*
 * Streaming mode state. Network chunks can split SSE lines anywhere,
 * so `line` reassembles them; `raw` keeps the full body so a non-SSE
 * error payload (e.g. a plain JSON 401) can still be diagnosed after
 * the transfer; `reply` accumulates the assistant text for the caller.
 */
typedef struct {
    Buffer line;
    Buffer raw;
    Buffer reply;
    Spinner *spinner; /* cleared as soon as content starts arriving */
    int    printing;  /* echo tokens to stdout as they arrive */
    int    got_error; /* saw an in-stream error event         */
    int    oom;       /* allocation failure mid-stream        */
} StreamState;

static void stream_stop_spinner(StreamState *st)
{
    spinner_stop(st->spinner);
    st->spinner = NULL;
}

/* Handle one complete SSE line. */
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
        /* Buffered Markdown keeps spinning until the full response;
         * live text clears the spinner before its first token. */
        if (st->printing)
            stream_stop_spinner(st);
        if (st->printing) {
            fputs(piece, stdout);
            fflush(stdout);
        }
        if (buf_append_str(&st->reply, piece) != 0)
            st->oom = 1;
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
        st->got_error = 1;
        free(err);
    }
}

static size_t sse_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    StreamState *st = ud;
    size_t n = size * nmemb;
    if (buf_append(&st->raw, ptr, n) != 0 ||
        buf_append(&st->line, ptr, n) != 0)
        return 0;

    /* Dispatch every complete line in the accumulator. */
    char *start = st->line.data;
    char *nl;
    while ((nl = memchr(start, '\n', st->line.len -
                        (size_t)(start - st->line.data))) != NULL) {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r')
            nl[-1] = '\0';
        sse_line(st, start);
        start = nl + 1;
    }

    /* Keep the unfinished tail for the next callback. */
    size_t rest = st->line.len - (size_t)(start - st->line.data);
    memmove(st->line.data, start, rest);
    st->line.len = rest;
    st->line.data[rest] = '\0';

    return st->oom ? 0 : n;
}

/* ------------------------------------------------------------------ */
/* Request                                                             */
/* ------------------------------------------------------------------ */

/* Report an error payload (JSON if parseable, raw otherwise). */
static void report_http_error(long status, const char *body)
{
    char *err = body ? extract_error(body) : NULL;
    if (err) {
        fprintf(stderr, "error (HTTP %ld): %s\n", status, err);
        free(err);
    } else {
        fprintf(stderr, "error (HTTP %ld): unexpected response:\n%s\n",
                status, body && *body ? body : "(empty)");
    }
}

int or_chat(const OrRequest *req, char **reply)
{
    *reply = NULL;

    /* Assemble the request body. */
    char *esc_model = json_escape(req->model);
    if (!esc_model) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }
    Buffer body = {0};
    int bad = buf_append_str(&body, "{\"model\":\"") ||
              buf_append_str(&body, esc_model)      ||
              buf_append_str(&body, "\",")          ||
              (req->stream &&
               buf_append_str(&body, "\"stream\":true,")) ||
              buf_append_str(&body, "\"messages\":")      ||
              buf_append_str(&body, req->messages_json)   ||
              buf_append_str(&body, "}");
    free(esc_model);
    if (bad) {
        fprintf(stderr, "error: out of memory\n");
        buf_free(&body);
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

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) {
        fprintf(stderr, "error: out of memory\n");
        curl_easy_cleanup(curl);
        buf_free(&body);
        return -1;
    }

    Buffer resp = {0};
    StreamState st = {0};
    st.printing = req->stream && !req->quiet;
    Spinner *spinner = req->spinner ? spinner_start("Thinking") : NULL;
    if (req->stream)
        st.spinner = spinner;

    curl_easy_setopt(curl, CURLOPT_URL, ORC_API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orc/0.2 (native C)");
    if (req->stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    }

    CURLcode rc = curl_easy_perform(curl);
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
            st.reply.data = NULL;
            result = 0;
        } else if (!st.got_error) {
            /* Nothing streamed and no SSE error: the server most
             * likely replied with a plain JSON error body. */
            report_http_error(http_status, st.raw.data);
        }
    } else {
        char *content = resp.data ? extract_content(resp.data) : NULL;
        if (content) {
            if (!req->quiet)
                printf("%s\n", content);
            *reply = content;
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
