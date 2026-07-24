/*
 * api.c — OpenRouter chat completions transport (libcurl + SSE).
 *
 * Two request shapes share one code path:
 *   - single-shot: collect the whole body, then extract the content;
 *   - streaming (SSE): reassemble `data:` lines as they arrive, echo
 *     assistant tokens to stdout, and accumulate the full reply.
 * Every exit frees the curl handle, the header slist, and all buffers.
 */
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <curl/curl.h>

#include "api.h"
#include "buffer.h"
#include "jsonutil.h"
#include "spinner.h"

/* Overall transfer deadline, in seconds. */
constexpr long ORC_TIMEOUT_SECS = 300;

/*
 * App attribution (https://openrouter.ai/docs/app-attribution): the
 * Referer URL is the app's primary identifier in OpenRouter's activity
 * logs and rankings — without it usage shows as "Unknown" — and the
 * title sets the display name.
 */
#define ORC_ATTRIB_REFERER \
    "HTTP-Referer: https://github.com/abhignan-rakshith/my-openrouter-client"
#define ORC_ATTRIB_TITLE "X-OpenRouter-Title: orc"

/*
 * Route a diagnostic to stderr, or into the caller's error sink when
 * one was supplied — callers streaming on the alternate screen collect
 * errors there and print them once the real screen is back, since
 * anything written during the stream is wiped with the screen.
 */
static void emit_error(Buffer *errs, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (!errs) {
        vfprintf(stderr, fmt, ap);
    } else {
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(nullptr, 0, fmt, ap);
        if (n >= 0) {
            char *tmp = malloc((size_t)n + 1);
            if (tmp) {
                vsnprintf(tmp, (size_t)n + 1, fmt, ap2);
                buf_append_str(errs, tmp);
                free(tmp);
            }
        }
        va_end(ap2);
    }
    va_end(ap);
}

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
    Buffer    *errs;      /* error sink from the request, or NULL       */
    Buffer    *images;    /* generated-image data URLs sink, or NULL    */
    bool       cancelled; /* user pressed Esc; keep the partial reply   */
} StreamState;

/*
 * Append each image_url.url in the "images" array reachable from `scope`
 * (a delta or message object) to `out`, one per line. Uses the
 * depth-aware iterator so a data URL's contents can't mis-split the
 * array. No-op when there is no images array.
 */
static void collect_images(const char *scope, Buffer *out)
{
    const char *arr = json_find_key(scope, "images");
    if (!arr || *arr != '[')
        return;
    const char *cur = arr;
    char *obj;
    while ((obj = json_array_next_object(&cur)) != nullptr) {
        const char *iu = json_find_key(obj, "image_url");
        const char *u = iu ? json_find_key(iu, "url") : nullptr;
        if (u && *u == '"') {
            char *url = json_decode_string(u, nullptr);
            if (url) {
                if (buf_append_str(out, url) == 0)
                    (void)buf_append_str(out, "\n");
                free(url);
            }
        }
        free(obj);
    }
}

static void stream_stop_spinner(StreamState *st)
{
    spinner_stop(st->spinner);
    st->spinner = nullptr;
}

/* Defined with the request path below; also used for SSE error events. */
static void report_api_error(Buffer *errs, long code, long retry_after,
                             const char *json);

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

    /* Generated images ride in delta.images, on a chunk whose content is
     * empty — so collect them before the empty-content early return. */
    if (st->images) {
        const char *choices = json_find_key(data, "choices");
        const char *delta = choices ? json_find_key(choices, "delta") : nullptr;
        if (delta)
            collect_images(delta, st->images);
    }

    char *piece = extract_delta(data);
    if (piece) {
        /* Providers emit empty content deltas before the first real token
         * (an initial role announcement, keep-alives). Ignore them: acting
         * on one would stop the spinner ~a second early and leave a silent
         * gap. Keep spinning until actual text arrives. */
        if (*piece) {
            if (buf_append_str(&st->reply, piece) != 0)
                st->oom = true;
            /* First visible content clears the spinner. A live-render
             * callback takes over display (fed the whole reply so far);
             * otherwise, when printing, echo just the new token. In
             * quiet/buffered mode both are off and the spinner runs until
             * the transfer completes. */
            if (!st->oom && st->on_update) {
                stream_stop_spinner(st);
                st->on_update(st->reply.data, st->on_update_user);
            } else if (!st->oom && st->printing) {
                stream_stop_spinner(st);
                fputs(piece, stdout);
                fflush(stdout);
            }
        }
        free(piece);
        return;
    }

    /* No delta content: either a housekeeping chunk (role, usage,
     * finish_reason) or an in-stream error event. */
    char *err = extract_error(data);
    if (err) {
        /* Mid-stream error event: HTTP 200 is already committed, so
         * the real code travels in the JSON (finish_reason "error"). */
        stream_stop_spinner(st);
        if (st->printing && st->reply.len)
            fputc('\n', stdout);
        report_api_error(st->errs, 0, 0, data);
        st->got_error = true;
        free(err);
    }
}

/*
 * Progress callback while esc_cancel is armed: poll stdin (which the
 * caller holds in raw mode) for a bare Esc keypress. curl invokes this
 * regularly even when no data is arriving, so a stalled stream can
 * still be interrupted. Escape *sequences* (arrow keys, pastes) start
 * with the same byte but arrive with a payload; only a lone 0x1b read
 * cancels. Other typed input during the stream is discarded. Returning
 * nonzero aborts the transfer (CURLE_ABORTED_BY_CALLBACK).
 */
static int esc_poll_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    StreamState *st = ud;
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    while (poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN)) {
        char b[64];
        ssize_t n = read(STDIN_FILENO, b, sizeof b);
        if (n <= 0)
            break;
        if (n == 1 && b[0] == '\033') {
            st->cancelled = true;
            return 1;
        }
    }
    return 0;
}

/* Capture the Retry-After response header (seconds) for 429/503. */
static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    long *retry_after = ud;
    size_t n = size * nmemb;
    if (n > 12 && strncasecmp(ptr, "Retry-After:", 12) == 0)
        *retry_after = strtol(ptr + 12, nullptr, 10);
    return n;
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

/*
 * An actionable next step for the well-known OpenRouter statuses
 * (https://openrouter.ai/docs/api_reference/errors-and-debugging), or
 * NULL when the error message speaks for itself.
 */
static const char *status_hint(long status)
{
    switch (status) {
    case 401: return "check your API key: orc config set key <API_KEY>";
    case 402: return "add credits to your OpenRouter account";
    case 408: return "the request timed out; try again";
    case 429: return "rate limited; wait a moment or switch models with -m";
    case 502: return "provider trouble; retry, or switch models with -m";
    case 503: return "no provider available right now; retry shortly";
    default:  return nullptr;
    }
}

/*
 * Diagnose an API error payload: "error (HTTP <code>[, <error_type>]):
 * <message>", plus an actionable hint for well-known statuses and the
 * server's Retry-After when it sent one. `code` 0 means "unknown"
 * (used for mid-stream events whose code lives in the JSON instead).
 * A 200 with no extractable error means the model produced no content
 * at all — per the docs, likely warm-up; suggest retrying.
 */
static void report_api_error(Buffer *errs, long code, long retry_after,
                             const char *json)
{
    char *msg  = json ? extract_error(json) : nullptr;
    char *type = json ? extract_error_type(json) : nullptr;
    if (code == 0 && json)
        code = extract_error_code(json);

    if (msg && code) {
        emit_error(errs, "error (HTTP %ld%s%s): %s\n", code,
                   type ? ", " : "", type ? type : "", msg);
    } else if (msg) {
        emit_error(errs, "error%s%s: %s\n",
                   type ? ", " : "", type ? type : "", msg);
    } else if (code == 200) {
        emit_error(errs, "error: the model returned no content "
                         "(it may be warming up); try again\n");
    } else {
        emit_error(errs, "error (HTTP %ld): unexpected response:\n%s\n",
                   code, json && *json ? json : "(empty)");
    }

    const char *hint = status_hint(code);
    if (hint && retry_after > 0)
        emit_error(errs, "  (%s; server says retry after %lds)\n",
                   hint, retry_after);
    else if (hint)
        emit_error(errs, "  (%s)\n", hint);
    else if (retry_after > 0)
        emit_error(errs, "  (server says retry after %lds)\n", retry_after);

    free(msg);
    free(type);
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
        emit_error(req->errs, "error: out of memory\n");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        emit_error(req->errs, "error: curl init failed\n");
        buf_free(&body);
        return -1;
    }

    char auth_header[512];
    int hn = snprintf(auth_header, sizeof auth_header,
                      "Authorization: Bearer %s", req->api_key);
    if (hn < 0 || (size_t)hn >= sizeof auth_header) {
        emit_error(req->errs, "error: API key is implausibly long\n");
        curl_easy_cleanup(curl);
        buf_free(&body);
        return -1;
    }

    struct curl_slist *headers = nullptr, *h = nullptr;
    static const char *const fixed[] = {
        "Content-Type: application/json",
        ORC_ATTRIB_REFERER,
        ORC_ATTRIB_TITLE,
    };
    h = curl_slist_append(headers, auth_header);
    for (size_t i = 0; h && i < sizeof fixed / sizeof *fixed; i++)
        h = curl_slist_append(headers = h, fixed[i]);
    if (!h) {
        emit_error(req->errs, "error: out of memory\n");
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        buf_free(&body);
        return -1;
    }
    headers = h;

    Buffer      resp = {0};
    StreamState st   = {0};
    st.on_update = req->on_update;
    st.on_update_user = req->on_update_user;
    st.errs = req->errs;
    st.images = req->images;
    /* The callback owns display when present, so plain echo is off then. */
    st.printing = req->stream && !req->quiet && !st.on_update;
    Spinner *spinner = req->spinner ? spinner_start("Thinking") : nullptr;
    if (req->stream)
        st.spinner = spinner;

    long retry_after = 0;
    curl_easy_setopt(curl, CURLOPT_URL, ORC_API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retry_after);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ORC_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orc/0.2 (native C)");
    /* The spinner runs in a second thread; disable curl's use of signals
     * so its timeout handling stays thread-safe. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (req->stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
        if (req->esc_cancel) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, esc_poll_cb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &st);
        }
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
    if (st.cancelled) {
        /* User pressed Esc. A partial reply counts as the result so it
         * can be rendered and persisted; with nothing received yet the
         * turn simply fails (the caller rolls it back). */
        if (st.reply.len) {
            if (st.printing)
                fputc('\n', stdout);
            emit_error(req->errs,
                       "note: interrupted; partial reply kept\n");
            *reply = st.reply.data; /* transfer ownership */
            st.reply.data = nullptr;
            result = 0;
        } else {
            emit_error(req->errs,
                       "note: interrupted before any reply arrived\n");
        }
    } else if (rc != CURLE_OK) {
        if (st.printing && st.reply.len)
            fputc('\n', stdout);
        emit_error(req->errs, "error: request failed: %s\n",
                   st.oom ? "out of memory" : curl_easy_strerror(rc));
    } else if (req->stream) {
        bool have_img = req->images && req->images->len;
        if ((st.reply.len || have_img) && !st.got_error) {
            if (st.printing)
                fputc('\n', stdout);
            /* An image-only reply has no text; hand back an empty string
             * so the contract (non-NULL on success) holds. */
            *reply = st.reply.data ? st.reply.data : strdup("");
            st.reply.data = nullptr;
            result = *reply ? 0 : -1;
        } else if (st.got_error) {
            /* The SSE error event was already reported as it arrived. */
            if (st.reply.len)
                emit_error(req->errs,
                           "note: the partial reply was discarded\n");
        } else {
            /* Nothing streamed and no SSE error: the server most likely
             * replied with a plain JSON error body. */
            report_api_error(req->errs, http_status, retry_after,
                             st.raw.data);
        }
    } else {
        char *content = resp.data ? extract_content(resp.data) : nullptr;
        if (req->images && resp.data) {
            const char *choices = json_find_key(resp.data, "choices");
            const char *msg = choices ? json_find_key(choices, "message")
                                      : nullptr;
            if (msg)
                collect_images(msg, req->images);
        }
        bool have_img = req->images && req->images->len;
        if (content || have_img) {
            if (!content)
                content = strdup("");   /* image-only reply: empty text */
            if (content && !req->quiet && *content)
                printf("%s\n", content);
            *reply = content; /* transfer ownership (may be "") */
            result = content ? 0 : -1;
        } else {
            report_api_error(req->errs, http_status, retry_after,
                             resp.data);
        }
    }

    buf_free(&resp);
    buf_free(&st.line);
    buf_free(&st.raw);
    buf_free(&st.reply);
    return result;
}

/* ------------------------------------------------------------------ */
/* Model capability preflight                                          */
/* ------------------------------------------------------------------ */

int or_model_supports_images(const char *api_key, const char *model)
{
    /* Per-model endpoint metadata; small response, includes
     * architecture.input_modalities. */
    char url[512];
    int n = snprintf(url, sizeof url, ORC_API_MODELS_URL "/%s/endpoints",
                     model);
    if (n < 0 || (size_t)n >= sizeof url)
        return -1;

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    char auth_header[512];
    int hn = snprintf(auth_header, sizeof auth_header,
                      "Authorization: Bearer %s", api_key);
    struct curl_slist *headers = (hn > 0 && (size_t)hn < sizeof auth_header)
        ? curl_slist_append(nullptr, auth_header)
        : nullptr;

    Buffer resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orc/0.2 (native C)");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int result = -1;
    if (rc == CURLE_OK && status == 200 && resp.data) {
        const char *v = json_find_key(resp.data, "input_modalities");
        const char *close = v && *v == '[' ? strchr(v, ']') : nullptr;
        if (close) {
            result = 0;
            for (const char *p = v; p + 7 <= close; p++) {
                if (strncmp(p, "\"image\"", 7) == 0) {
                    result = 1;
                    break;
                }
            }
        }
    }
    buf_free(&resp);
    return result;
}

int or_models_fetch(const char *api_key, Buffer *out)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "error: could not initialize HTTP client\n");
        return -1;
    }

    char auth_header[512];
    int hn = snprintf(auth_header, sizeof auth_header,
                      "Authorization: Bearer %s", api_key);
    struct curl_slist *headers = (hn > 0 && (size_t)hn < sizeof auth_header)
        ? curl_slist_append(nullptr, auth_header)
        : nullptr;

    /* output_modalities=text: only models orc can render; limit=1000:
     * comfortably above the current count, so one page suffices. */
    curl_easy_setopt(curl, CURLOPT_URL, ORC_API_MODELS_URL
                     "?output_modalities=text&limit=1000");
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orc/0.2 (native C)");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        fprintf(stderr, "error: could not fetch model list: %s\n",
                curl_easy_strerror(rc));
        buf_free(out);
        return -1;
    }
    if (status != 200 || !out->data) {
        fprintf(stderr, "error: model list request failed (HTTP %ld)\n",
                status);
        buf_free(out);
        return -1;
    }
    return 0;
}
