/*
 * main.c — orc, a minimal OpenRouter chat CLI in native C.
 *
 * Modes:
 *   orc "prompt"              one-shot question (not persisted)
 *   orc                       interactive chat, auto-named conversation
 *   orc -c NAME               resume (or start) conversation NAME
 *   orc -c NAME "prompt"      one turn appended to conversation NAME
 *   orc config ...            manage persistent settings (key, model)
 *
 * Options:
 *   -m MODEL      model id (default: DEFAULT_MODEL)
 *   --no-stream   wait for the full response instead of streaming
 *   --no-markdown stream plain text only; skip the Markdown re-render
 *
 * In an interactive terminal the reply streams onto the alternate screen,
 * rendered live as Markdown as it arrives; once complete that view is
 * dropped and the finished reply is drawn once on the main screen, so
 * only the final formatted copy remains in the scrollback.
 *
 * Interactive commands:
 *   /help         show commands and key bindings (on the alt screen)
 *   /model        switch the model via a searchable picker (starrable)
 *   /rename NAME  rename the current conversation
 *   /save [NAME]  export the last reply to saves/<name>.md
 *   /quit         end the session
 *
 * Ctrl+V pastes an image from the clipboard into the prompt: the input
 * shows a [Image N] placeholder and the image is sent alongside the
 * text as multimodal content (vision-capable models only; support is
 * checked against the OpenRouter model metadata on first paste).
 *
 * Images a model generates are saved under conversations/generated/ and
 * rendered inline via chafa (any format; a saved-path note when chafa is
 * absent); an [image: path] marker is kept in the conversation text so a
 * resumed conversation re-renders them (see image.c).
 *
 * Conversations are stored in conversations/<name>.jsonl, one
 * {"role":...,"content":...} object per line.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <unistd.h>

#include "api.h"
#include "buffer.h"
#include "clipboard.h"
#include "config.h"
#include "conv.h"
#include "image.h"
#include "jsonutil.h"
#include "lineedit.h"
#include "md.h"
#include "models.h"
#include "save.h"
#include "screen.h"
#include "userconfig.h"

#define DEFAULT_MODEL "google/gemini-3.6-flash"

/* Longest conversation on-disk path we build (matches conv module scope). */
constexpr size_t PATH_CAP = 512;
/* Longest auto-generated conversation name (chat-YYYYMMDD-HHMMSS + slack). */
constexpr size_t NAME_CAP = 64;

/* Where pasted clipboard images are saved before being inlined. */
#define ORC_ATTACH_DIR ORC_CONV_DIR "/attachments"

/* Where model-generated images are saved. */
#define ORC_GENERATED_DIR ORC_CONV_DIR "/generated"

/* Most images attachable to a single message. */
constexpr size_t MAX_PENDING_IMAGES = 16;

/* Parsed command-line options. */
typedef struct {
    const char *model;
    const char *prompt;     /* nullptr => interactive session */
    const char *conv_name;  /* nullptr => no explicit -c name  */
    bool        stream;
    bool        markdown;
} Options;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-m model] [-c name] [--no-stream] [--no-markdown]"
        " [\"prompt\"]\n"
        "       %s config <set|get|unset|list|path> ...\n"
        "  -m model     model id (default: %s, or the saved config value)\n"
        "  -c name      persist to conversations/<name>.jsonl\n"
        "  --no-stream  wait for the full response instead of streaming\n"
        "  --no-markdown  stream plain text only; skip the Markdown re-render\n"
        "\n"
        "With no prompt, an interactive chat session starts. In a terminal the\n"
        "reply renders live as Markdown as it streams.\n"
        "In the chat, /help lists every command and key binding;\n"
        "/rename NAME renames the conversation and /save [NAME] exports\n"
        "the last reply to saves/<name>.md.\n"
        "/quit (or Ctrl-D) ends the session.\n"
        "\\+Enter (or Shift+Enter in supporting terminals) starts a new\n"
        "line without sending, for multi-line messages.\n"
        "Esc interrupts a streaming reply, keeping the partial text.\n"
        "Ctrl+V pastes an image from the clipboard ([Image N] placeholder);\n"
        "it is sent as multimodal content to vision-capable models.\n"
        "\n"
        "Persistent settings (~/.config/orc/config, 0600):\n"
        "  %s config set key <API_KEY>     save the OpenRouter API key\n"
        "  %s config set model <model-id>  save the default model\n"
        "  %s config get <key|model>       show a saved value (key masked)\n"
        "  %s config list                  show all saved settings\n"
        "  %s config unset <key|model>     remove a saved value\n"
        "  %s config path                  print the config file location\n",
        prog, prog, DEFAULT_MODEL, prog, prog, prog, prog, prog, prog);
}

/* Map a user-facing setting name to its stored key, or nullptr if unknown. */
static const char *config_key_for(const char *name)
{
    if (strcmp(name, "key") == 0)   return ORC_KEY_NAME;
    if (strcmp(name, "model") == 0) return "model";
    return nullptr;
}

/* Render a masked preview of a secret into out (e.g. "sk-or-…a1b2"). */
static void mask_secret(const char *s, char *out, size_t outsz)
{
    size_t n = strlen(s);
    if (n <= 8)
        snprintf(out, outsz, "********");
    else
        snprintf(out, outsz, "%.6s…%.4s", s, s + n - 4);
}

/* Print one setting for `config get`/`list`, masking the key. */
static void config_print(const char *label, const char *ui_name, char *value)
{
    if (!value) {
        printf("%s(not set)\n", label);
        return;
    }
    if (strcmp(ui_name, "key") == 0) {
        char masked[64];
        mask_secret(value, masked, sizeof masked);
        printf("%s%s\n", label, masked);
    } else {
        printf("%s%s\n", label, value);
    }
    free(value);
}

/*
 * Handle `orc config ...`. Returns a process exit code. Does not require an
 * API key, so it is dispatched before the normal startup path.
 */
static int run_config_cmd(int argc, char **argv)
{
    const char *sub = argc >= 3 ? argv[2] : "";

    if (strcmp(sub, "path") == 0) {
        char *p = uconf_path();
        if (!p) {
            fprintf(stderr, "error: no HOME or XDG_CONFIG_HOME set\n");
            return 1;
        }
        printf("%s\n", p);
        free(p);
        return 0;
    }

    if (strcmp(sub, "list") == 0) {
        config_print("key:   ", "key", uconf_get(ORC_KEY_NAME));
        config_print("model: ", "model", uconf_get("model"));
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: orc config set <key|model> <value>\n");
            return 1;
        }
        const char *stored = config_key_for(argv[3]);
        if (!stored) {
            fprintf(stderr, "error: unknown setting '%s' (use 'key' or 'model')\n", argv[3]);
            return 1;
        }
        if (uconf_set(stored, argv[4]) != 0)
            return 1;   /* uconf_set already reported the error */
        if (strcmp(argv[3], "key") == 0) {
            char masked[64];
            mask_secret(argv[4], masked, sizeof masked);
            printf("Saved API key (%s).\n", masked);
        } else {
            printf("Saved model = %s\n", argv[4]);
        }
        return 0;
    }

    if (strcmp(sub, "get") == 0) {
        if (argc < 4 || !config_key_for(argv[3])) {
            fprintf(stderr, "usage: orc config get <key|model>\n");
            return 1;
        }
        config_print("", argv[3], uconf_get(config_key_for(argv[3])));
        return 0;
    }

    if (strcmp(sub, "unset") == 0) {
        if (argc < 4 || !config_key_for(argv[3])) {
            fprintf(stderr, "usage: orc config unset <key|model>\n");
            return 1;
        }
        if (uconf_unset(config_key_for(argv[3])) != 0)
            return 1;
        printf("Unset %s.\n", argv[3]);
        return 0;
    }

    fprintf(stderr, "error: unknown config command '%s'\n", sub);
    usage(argv[0]);
    return 1;
}

/*
 * Parse argv into *opts. Returns true if main should continue; returns
 * false when it should exit immediately, storing the exit code in
 * *exit_code (0 for --help, 1 for a usage error).
 */
static bool parse_args(int argc, char **argv, Options *opts, int *exit_code)
{
    *opts = (Options){
        .model    = nullptr,   /* resolved after parsing: -m > config > default */
        .prompt   = nullptr,
        .conv_name = nullptr,
        .stream   = true,
        .markdown = true,
    };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-m") == 0 && i + 1 < argc) {
            opts->model = argv[++i];
        } else if (strcmp(arg, "-c") == 0 && i + 1 < argc) {
            opts->conv_name = argv[++i];
        } else if (strcmp(arg, "--no-stream") == 0) {
            opts->stream = false;
        } else if (strcmp(arg, "--no-markdown") == 0) {
            opts->markdown = false;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            *exit_code = 0;
            return false;
        } else if (arg[0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", arg);
            usage(argv[0]);
            *exit_code = 1;
            return false;
        } else if (!opts->prompt) {
            opts->prompt = arg;
        } else {
            fprintf(stderr, "error: multiple prompts given\n");
            usage(argv[0]);
            *exit_code = 1;
            return false;
        }
    }
    return true;
}

/* Print the "assistant>" banner shown before a rendered Markdown reply. */
static void print_assistant_banner(void)
{
    if (md_color_enabled())
        fputs("\033[1;38;5;141massistant>\033[0m\n", stdout);
    else
        fputs("assistant>\n", stdout);
}

/* The interactive "you> " prompt string (color when appropriate). */
static const char *you_prompt(bool markdown)
{
    return (markdown && md_color_enabled())
        ? "\033[1;38;5;84myou>\033[0m " : "you> ";
}

/*
 * Async-signal-safe handler for Ctrl-C and friends: if we were streaming on
 * the alternate screen, restore the main screen (a lone write(2) is safe in
 * a handler) so the terminal is never left stranded. Then re-raise with the
 * default disposition so the exit status still reflects the signal.
 */
static void on_fatal_signal(int sig)
{
    if (g_in_alt_screen) {
        ssize_t r = write(STDOUT_FILENO, "\033[?1049l", 8);
        (void)r;
    }
    le_signal_restore();    /* leave raw mode if the editor was active */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Install on_fatal_signal for the signals that would otherwise kill us
 * mid-stream and leave the terminal on the alternate screen. */
static void install_signal_handlers(void)
{
    struct sigaction sa = { .sa_handler = on_fatal_signal };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

/* Throttle state for the live streaming re-render. */
typedef struct { struct timespec last; bool started; } StreamPaint;

/*
 * or_chat progress callback: repaint the alternate screen with the reply
 * rendered as Markdown so far. Throttled to ~60ms so a fast token stream
 * doesn't thrash the terminal.
 *
 * Each frame clears the whole screen and redraws from the top. Merely
 * homing and overwriting the previous frame is not enough: once the
 * reply outgrows the terminal, printing it scrolls the alt screen, so
 * "home" no longer matches the document top and subsequent frames land
 * misaligned over stale rows (spliced/duplicated lines). The clear
 * makes residue impossible — a reply taller than the screen simply
 * scrolls so its tail (the newest text) stays visible. The frame is
 * bracketed in the synchronized-update protocol (ESC[?2026h/l) so
 * terminals that support it apply clear+redraw atomically, without
 * flicker; others ignore the sequences and just redraw fast.
 *
 * Partial constructs (an unclosed **bold**, a half-finished list) simply
 * settle as more text arrives — the nature of rendering Markdown live.
 */
static void stream_repaint(const char *reply, void *user)
{
    StreamPaint *sp = user;
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    if (sp->started) {
        long ms = (now.tv_sec - sp->last.tv_sec) * 1000
                + (now.tv_nsec - sp->last.tv_nsec) / 1000000;
        if (ms < 60)
            return;                     /* too soon: skip this frame */
    }
    sp->last = now;
    sp->started = true;

    fputs("\033[?2026h", stdout);       /* begin synchronized update */
    fputs("\033[2J\033[H", stdout);     /* clear all, then home */
    print_assistant_banner();
    md_render(reply, md_color_enabled());
    fputs("\033[?2026l", stdout);       /* end synchronized update */
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Clipboard image paste                                               */
/* ------------------------------------------------------------------ */

/* One clipboard image pasted but not yet sent. */
typedef struct {
    char *path;     /* saved PNG under ORC_ATTACH_DIR */
    int   tag;      /* N of its "[Image N]" placeholder */
} PendingImage;

/* Paste state threaded through the line editor's Ctrl+V callback. */
typedef struct {
    const char  *api_key;
    const char  *model;
    int          support;   /* -2 unknown, 0 no, 1 yes (or assumed)    */
    int          next_tag;  /* next [Image N] number, session-wide     */
    PendingImage imgs[MAX_PENDING_IMAGES];
    size_t       nimgs;
} PasteCtx;

/* Replace *msg (freeing any previous) with a formatted status line. */
static void set_msg(char **msg, const char *fmt, const char *arg)
{
    free(*msg);
    char buf[256];
    snprintf(buf, sizeof buf, fmt, arg);
    *msg = strdup(buf);
}

/*
 * Ctrl+V handler: verify the model can take images (once, cached for
 * the session), pull the clipboard image into the attachments dir, and
 * hand the editor a "[Image N]" placeholder to splice into the line.
 */
static char *paste_cb(void *user, char **msg)
{
    PasteCtx *pc = user;
    *msg = nullptr;

    if (pc->support == -2) {
        int s = or_model_supports_images(pc->api_key, pc->model);
        if (s == -1) {
            set_msg(msg, "warning: could not verify image support for "
                         "model '%s'; sending anyway", pc->model);
            s = 1;      /* optimistic: let the API be the judge */
        }
        pc->support = s;
    }
    if (pc->support == 0) {
        set_msg(msg, "error: model '%s' does not support image input "
                     "(try a vision model, e.g. " DEFAULT_MODEL ")",
                pc->model);
        return nullptr;
    }
    if (pc->nimgs >= MAX_PENDING_IMAGES) {
        set_msg(msg, "error: too many images in one message%s", "");
        return nullptr;
    }

    if (conv_ensure_dir() != 0 ||
        (mkdir(ORC_ATTACH_DIR, 0755) != 0 && errno != EEXIST)) {
        set_msg(msg, "error: cannot create %s/", ORC_ATTACH_DIR);
        return nullptr;
    }

    char path[PATH_CAP];
    snprintf(path, sizeof path, ORC_ATTACH_DIR "/img-%ld-%d.png",
             (long)time(nullptr), pc->next_tag);
    int shot = clip_image_save(path);
    if (shot != 0) {
        if (shot < 0)
#if defined(__APPLE__)
            set_msg(msg, "error: could not read the clipboard%s", "");
#else
            set_msg(msg, "error: no clipboard helper found "
                         "(install wl-clipboard or xclip)%s", "");
#endif
        else
            set_msg(msg, "no image found on the clipboard%s", "");
        return nullptr;
    }

    char *saved = strdup(path);
    if (!saved) {
        remove(path);
        return nullptr;
    }
    pc->imgs[pc->nimgs] = (PendingImage){ .path = saved,
                                          .tag = pc->next_tag };
    pc->nimgs++;

    char tagtxt[32];
    snprintf(tagtxt, sizeof tagtxt, "[Image %d]", pc->next_tag);
    pc->next_tag++;
    return strdup(tagtxt);
}

/* Forget (but keep on disk) any images pasted for the current line. */
static void clear_pending(PasteCtx *pc)
{
    for (size_t i = 0; i < pc->nimgs; i++)
        free(pc->imgs[i].path);
    pc->nimgs = 0;
}

/*
 * Build a multimodal content array (JSON value) for `text` plus every
 * pending image whose [Image N] placeholder still appears in the text
 * (deleting the placeholder de-attaches the image). Returns a malloc'd
 * JSON array, or NULL when no image is attached / on failure —
 * distinguished via *oom.
 */
static char *build_mm_content(PasteCtx *pc, const char *text, bool *oom)
{
    *oom = false;
    char *esc = json_escape(text);
    if (!esc) {
        *oom = true;
        return nullptr;
    }

    Buffer out = {0};
    bool bad = buf_append_str(&out, "[{\"type\":\"text\",\"text\":\"") ||
               buf_append_str(&out, esc) ||
               buf_append_str(&out, "\"}");
    free(esc);

    size_t attached = 0;
    for (size_t i = 0; i < pc->nimgs && !bad; i++) {
        char tagtxt[32];
        snprintf(tagtxt, sizeof tagtxt, "[Image %d]", pc->imgs[i].tag);
        if (!strstr(text, tagtxt))
            continue;   /* placeholder edited out: don't send it */

        char *url = clip_file_data_url(pc->imgs[i].path);
        if (!url) {
            fprintf(stderr, "warning: cannot read %s, skipping image\n",
                    pc->imgs[i].path);
            continue;
        }
        bad = buf_append_str(&out,
                  ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"") ||
              buf_append_str(&out, url) ||
              buf_append_str(&out, "\"}}");
        free(url);
        attached++;
    }
    bad = bad || buf_append_str(&out, "]");

    if (bad || attached == 0) {
        *oom = bad;
        buf_free(&out);
        return nullptr;
    }
    return out.data;    /* ownership moves to the caller */
}

/*
 * Undo the user-message append after a failed exchange, so a message
 * the model never answered doesn't poison the next request or persist
 * in the conversation file. `len0`/`fsize0` are the history sizes
 * captured before the append (fsize0 < 0: the file didn't exist yet).
 */
static void rollback_turn(Buffer *items, size_t len0,
                          const char *path, off_t fsize0)
{
    items->len = len0;
    if (items->data)
        items->data[len0] = '\0';
    if (!path)
        return;
    if (fsize0 < 0)
        remove(path);
    else if (truncate(path, fsize0) != 0)
        fprintf(stderr, "warning: could not remove the failed message "
                        "from %s\n", path);
}

/*
 * Save and render any generated images returned in `images` (newline-
 * separated data URLs), appending an "[image: <path>]" marker for each to
 * `disp` — the text that gets persisted, so a resumed conversation shows
 * the reference. Images render inline on Kitty-protocol terminals,
 * otherwise the saved path is printed. Best-effort: a save/render failure
 * is reported but does not fail the turn.
 */
static void render_generated_images(Buffer *images, Buffer *disp, bool color)
{
    if (!images->len)
        return;
    if (conv_ensure_dir() != 0 ||
        (mkdir(ORC_GENERATED_DIR, 0755) != 0 && errno != EEXIST)) {
        fprintf(stderr, "warning: cannot create %s/\n", ORC_GENERATED_DIR);
        return;
    }

    char *urls = strdup(images->data);
    if (!urls)
        return;
    long ts = (long)time(nullptr);
    int idx = 0;
    for (char *url = strtok(urls, "\n"); url; url = strtok(nullptr, "\n")) {
        idx++;
        char *img = img_save_data_url(url, ORC_GENERATED_DIR, ts, idx);
        if (!img) {
            fprintf(stderr, "warning: could not save a generated image\n");
            continue;
        }
        bool shown = img_render_file(img);
        const char *fmt = shown
            ? (color ? "\033[2m(saved: %s)\033[0m\n" : "(saved: %s)\n")
            : "[image saved: %s]\n";
        printf(fmt, img);

        char marker[PATH_CAP + 16];
        snprintf(marker, sizeof marker, "%s[image: %s]",
                 disp->len ? "\n" : "", img);
        (void)buf_append_str(disp, marker);
        free(img);
    }
    free(urls);
}

/*
 * Run one exchange: append the user message to the in-memory history
 * (and file, if any), call the API with the full history, then record
 * the assistant's reply. Returns 0 on success, -1 on failure — the
 * unanswered user message is rolled back from memory and disk.
 *
 * In an interactive color terminal the reply streams in as plain text on
 * the alternate screen for responsiveness; once complete that view is
 * dropped and the finished reply is re-drawn with Markdown styling on the
 * main screen, so only the formatted copy remains.
 *
 * `content_json`, when non-NULL, is a pre-encoded multimodal content
 * array (text + images) recorded and sent in place of the plain
 * user_msg string.
 *
 * `out_reply`, when non-NULL, receives ownership of the assistant's reply
 * string on success (any previous *out_reply is freed first), so the
 * caller can retain the most recent reply — e.g. for /save. On failure it
 * is left untouched. When NULL the reply is simply freed.
 */
static int run_turn(const OrRequest *base, Buffer *items,
                    const char *path, const char *user_msg,
                    const char *content_json, char **out_reply)
{
    /* Snapshot the history so a failed exchange can be rolled back. */
    size_t len0   = items->len;
    off_t  fsize0 = -1;
    if (path) {
        struct stat stb;
        if (stat(path, &stb) == 0)
            fsize0 = stb.st_size;
    }

    int arc = content_json
        ? conv_items_add_json(items, "user", content_json)
        : conv_items_add(items, "user", user_msg);
    if (arc != 0) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }
    if (path) {
        arc = content_json
            ? conv_append_json(path, "user", content_json)
            : conv_append(path, "user", user_msg);
        if (arc != 0) {
            rollback_turn(items, len0, path, fsize0);
            return -1;
        }
    }

    /* Wrap the accumulated items in a JSON array. */
    Buffer msgs = {0};
    if (buf_append_str(&msgs, "[")         ||
        buf_append_str(&msgs, items->data) ||
        buf_append_str(&msgs, "]")) {
        fprintf(stderr, "error: out of memory\n");
        buf_free(&msgs);
        rollback_turn(items, len0, path, fsize0);
        return -1;
    }

    OrRequest req = *base;
    req.messages_json = msgs.data;

    /* Collect any images an image-output model returns in the reply. */
    Buffer gen_images = {0};
    req.images = &gen_images;

    /* render == re-render the finished reply with Markdown styling.
     * base->markdown is already gated to an interactive color terminal.
     * `live` streams the plain tokens on the alternate screen; otherwise
     * there is nothing to show live, so suppress or_chat's own plain print
     * and render the buffered reply instead. */
    bool render = req.markdown;
    bool live   = render && req.stream;
    if (render && !req.stream)
        req.quiet = 1;

    StreamPaint paint = {0};
    Buffer errs = {0};
    if (live) {
        /* Stream into the alternate screen, rendered live as Markdown; a
         * transient banner heads it until the first repaint. Errors are
         * collected rather than printed: anything written to the
         * alternate screen is wiped when it is left. */
        screen_alt_enter();
        print_assistant_banner();
        fflush(stdout);
        req.on_update = stream_repaint;
        req.on_update_user = &paint;
        req.errs = &errs;
    }

    /* Esc interrupts a streaming reply, keeping the partial text. Raw
     * mode makes the keypress readable immediately (and stops stray
     * typing echoing over the stream); the fatal-signal path restores
     * the terminal if we die mid-stream. */
    if (req.stream && le_raw_on() == 0)
        req.esc_cancel = 1;

    char *reply = nullptr;
    int rc = or_chat(&req, &reply);

    if (req.esc_cancel)
        le_raw_off();
    if (live)               /* drop the raw stream; back to the main screen */
        screen_alt_leave();
    if (errs.len)           /* now safe to show what went wrong */
        fputs(errs.data, stderr);
    buf_free(&errs);

    buf_free(&msgs);
    if (rc != 0) {
        buf_free(&gen_images);
        rollback_turn(items, len0, path, fsize0);
        return -1;
    }

    if (render) {
        print_assistant_banner();
        md_render(reply, md_color_enabled());
    }

    /* Persist the text plus an [image: path] marker per generated image
     * (saved and, on Kitty terminals, rendered inline here). */
    Buffer disp = {0};
    (void)buf_append_str(&disp, reply);
    render_generated_images(&gen_images, &disp, md_color_enabled());
    buf_free(&gen_images);
    const char *persist = disp.data ? disp.data : reply;

    rc = 0;
    if (conv_items_add(items, "assistant", persist) != 0) {
        fprintf(stderr, "error: out of memory\n");
        rc = -1;
    } else if (path && conv_append(path, "assistant", persist) != 0) {
        rc = -1;
    }
    buf_free(&disp);
    if (out_reply) {
        free(*out_reply);       /* drop the previous turn's retained reply */
        *out_reply = reply;     /* hand ownership to the caller           */
    } else {
        free(reply);
    }
    return rc;
}

/* True if `line` is `cmd`, optionally followed by whitespace + args. */
static bool is_command(const char *line, const char *cmd, size_t cmd_len)
{
    return strncmp(line, cmd, cmd_len) == 0 &&
           (line[cmd_len] == '\0' || line[cmd_len] == ' ' ||
            line[cmd_len] == '\t');
}

/* Skip leading spaces and tabs. */
static char *skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/*
 * Handle the /rename command. `args` points just past "/rename".
 * On success updates *path in place (buffer of pathsz bytes).
 */
static void handle_rename(char *args, char *path, size_t pathsz)
{
    char *name = skip_ws(args);
    if (!*name) {
        fprintf(stderr, "usage: /rename NAME\n");
        return;
    }

    char newpath[PATH_CAP];
    if (!path || conv_path(name, newpath, sizeof newpath) != 0) {
        fprintf(stderr, "error: invalid conversation name '%s'\n", name);
        return;
    }
    if (strlen(newpath) + 1 > pathsz) {
        fprintf(stderr, "error: conversation name is too long\n");
        return;
    }
    if (conv_rename(path, newpath) != 0)
        return;

    memcpy(path, newpath, strlen(newpath) + 1);
    printf("Conversation renamed to %s\n", path);
}

/*
 * Handle the /save command. `args` points just past "/save"; an optional
 * name follows, otherwise a timestamped default is used. Writes the last
 * assistant reply to saves/<name>.md. `last_reply` is NULL when no reply
 * has been received yet this session.
 */
static void handle_save(char *args, const char *last_reply)
{
    if (!last_reply) {
        fprintf(stderr, "Nothing to save yet — no reply this session.\n");
        return;
    }

    char namebuf[NAME_CAP];
    char *name = skip_ws(args);
    if (!*name) {
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(namebuf, sizeof namebuf, "reply-%Y%m%d-%H%M%S", &tm);
        name = namebuf;
    }

    char path[PATH_CAP];
    if (save_path(name, path, sizeof path) != 0) {
        fprintf(stderr, "error: invalid save name '%s'\n", name);
        return;
    }
    if (save_ensure_dir() != 0 || save_write(path, last_reply) != 0)
        return;
    printf("Saved reply to %s\n", path);
}

/* One row of the help page: a key/command and what it does. */
typedef struct { const char *key, *desc; } HelpRow;

/* Print a titled block of help rows, aligned, colored when appropriate.
 * Section titles use the assistant purple, keys the you-green, so the
 * page reads as part of the same interface. */
static void help_section(const char *title, const HelpRow *rows, size_t n,
                         bool color)
{
    if (color)
        printf("\033[1;38;5;141m%s\033[0m\n", title);
    else
        printf("%s\n", title);
    for (size_t i = 0; i < n; i++) {
        if (color)
            printf("  \033[1;38;5;84m%-20s\033[0m %s\n",
                   rows[i].key, rows[i].desc);
        else
            printf("  %-20s %s\n", rows[i].key, rows[i].desc);
    }
    putchar('\n');
}

/* Render the help page (used on the alternate screen, or inline when piped). */
static void print_help_page(const OrRequest *base, bool color)
{
    static const HelpRow commands[] = {
        { "/help, /?",     "show this help" },
        { "/model",        "switch the model (searchable, star favourites)" },
        { "/save [NAME]",  "export the last reply to saves/<name>.md" },
        { "/rename NAME",  "rename the current conversation" },
        { "/quit, /exit",  "end the session (also Ctrl-D)" },
    };
    static const HelpRow editing[] = {
        { "\\+Enter, S-Enter", "new line without sending" },
        { "Up / Down",         "recall history / move between lines" },
        { "Ctrl-A / Ctrl-E",   "jump to start / end of line" },
        { "Ctrl-K / Ctrl-U",   "delete to end / start of line" },
        { "Ctrl-W",            "delete the previous word" },
        { "Ctrl-L",            "clear the screen" },
        { "Ctrl-V",            "paste an image from the clipboard" },
        { "Esc",               "interrupt a streaming reply" },
    };

    if (color)
        printf("\033[1;38;5;141morc\033[0m — chatting with %s\n\n", base->model);
    else
        printf("orc — chatting with %s\n\n", base->model);
    help_section("Commands", commands,
                 sizeof commands / sizeof *commands, color);
    help_section("Editing & input", editing,
                 sizeof editing / sizeof *editing, color);
}

/*
 * Show the help page. On a terminal it is painted on the alternate screen
 * and dismissed with any key, so the conversation scrollback and history
 * are left completely undisturbed. When stdin/stdout is not a terminal it
 * is simply printed inline.
 */
static void show_help(const OrRequest *base)
{
    bool color = md_color_enabled();
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        print_help_page(base, color);
        return;
    }

    screen_alt_enter();
    print_help_page(base, color);
    if (color)
        fputs("\033[2mPress any key to return.\033[0m", stdout);
    else
        fputs("Press any key to return.", stdout);
    fflush(stdout);

    /* Wait for one keypress in raw mode, then restore the main screen —
     * the help page vanishes with the alternate buffer. */
    if (le_raw_on() == 0) {
        char c;
        while (read(STDIN_FILENO, &c, 1) < 0 && errno == EINTR)
            ;
        le_raw_off();
    }
    screen_alt_leave();
}

/*
 * Print a quiet one-line session banner — the model, and a pointer to
 * /help for everything else — then replay prior history when resuming.
 */
static int repl_intro(const OrRequest *base, const Buffer *items,
                      const char *path)
{
    bool color = md_color_enabled();
    if (color)
        printf("\033[1;38;5;141morc\033[0m \033[2m·\033[0m %s", base->model);
    else
        printf("orc · %s", base->model);
    if (isatty(STDIN_FILENO)) {
        const char *hint = "  /help for commands · /quit to exit";
        if (color)
            printf("\033[2m%s\033[0m", hint);
        else
            printf("%s", hint);
    }
    putchar('\n');

    if (path && items->len) {
        printf("\n--- Previous messages ---\n");
        if (conv_show(path, base->markdown) != 0)
            return 1;
        printf("\n--- Resuming chat ---\n");
    }
    return 0;
}

/* Interactive read–send–print loop. */
static int repl(OrRequest *base, Buffer *items, char *path, size_t pathsz)
{
    if (repl_intro(base, items, path) != 0)
        return 1;

    PasteCtx pc = {
        .api_key  = base->api_key,
        .model    = base->model,
        .support  = -2,
        .next_tag = 1,
    };

    /* Seed from disk so /save works on a resumed conversation, not just
     * after a fresh turn this session. */
    char *last_reply = path ? conv_last_reply(path) : nullptr;
    /* Owns the id when /model switches the active model this session
     * (base->model then points here). */
    char *model_override = nullptr;
    int status = 0;
    for (;;) {
        putchar('\n');
        char *line = le_readline(you_prompt(base->markdown),
                                 paste_cb, &pc);
        if (!line) { /* EOF */
            putchar('\n');
            break;
        }
        if (!*line) {
            free(line);
            continue;
        }

        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            free(line);
            break;
        }
        if (is_command(line, "/rename", 7)) {
            handle_rename(line + 7, path, pathsz);
            clear_pending(&pc);     /* pastes don't survive the command */
            free(line);
            continue;
        }
        if (is_command(line, "/save", 5)) {
            handle_save(line + 5, last_reply);
            clear_pending(&pc);     /* pastes don't survive the command */
            free(line);
            continue;
        }
        if (strcmp(line, "/help") == 0 || strcmp(line, "/?") == 0) {
            show_help(base);
            clear_pending(&pc);     /* pastes don't survive the command */
            free(line);
            continue;
        }
        if (strcmp(line, "/model") == 0) {
            bool outputs_images = false;
            char *picked = models_pick(base->api_key, base->model,
                                       &outputs_images);
            if (picked) {
                free(model_override);
                model_override = picked;
                base->model = model_override;
                pc.model    = base->model;  /* recheck image support anew */
                pc.support  = -2;
                /* Persist as the default so the choice survives restarts
                 * (config precedence: -m flag > saved model > built-in). */
                bool saved = uconf_set("model", base->model) == 0;
                if (md_color_enabled())
                    printf("Switched to \033[1;38;5;141m%s\033[0m%s\n",
                           base->model,
                           saved ? "  \033[2m(saved as default)\033[0m" : "");
                else
                    printf("Switched to %s%s\n", base->model,
                           saved ? "  (saved as default)" : "");
                if (outputs_images)
                    fprintf(stderr, "note: this model can generate images; "
                            "orc shows the text reply only.\n");
            }
            clear_pending(&pc);     /* pastes don't survive the command */
            free(line);
            continue;
        }

        /* Attach any pasted images whose placeholder survived editing. */
        char *mm = nullptr;
        bool oom = false;
        if (pc.nimgs)
            mm = build_mm_content(&pc, line, &oom);
        clear_pending(&pc);
        if (oom) {
            fprintf(stderr, "error: out of memory\n");
            free(line);
            status = 1;
            continue;
        }

        putchar('\n');
        if (run_turn(base, items, path, line, mm, &last_reply) != 0) {
            /* Report and keep the session alive; the user may retry. */
            status = 1;
        }
        free(mm);
        free(line);
    }
    clear_pending(&pc);
    free(last_reply);
    free(model_override);
    return status;
}

/* Generate a timestamp-based conversation name: chat-YYYYMMDD-HHMMSS. */
static void auto_name(char *out, size_t outsz)
{
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, outsz, "chat-%Y%m%d-%H%M%S", &tm);
}

/*
 * Resolve the conversation file, if persistence is in play. Interactive
 * sessions are always persisted (auto-named when the user didn't pick a
 * name); bare one-shots stay ephemeral. On success stores the resolved
 * path in `pathbuf` and returns a pointer to it (or nullptr for an
 * ephemeral one-shot). On error prints a diagnostic and returns nullptr
 * with *failed set to true.
 */
static char *resolve_path(const Options *opts, char *namebuf, size_t namesz,
                          char *pathbuf, size_t pathsz, bool *failed)
{
    *failed = false;
    const char *conv_name = opts->conv_name;
    if (!conv_name && !opts->prompt) {
        auto_name(namebuf, namesz);
        conv_name = namebuf;
    }
    if (!conv_name)
        return nullptr;

    if (conv_path(conv_name, pathbuf, pathsz) != 0) {
        fprintf(stderr, "error: invalid conversation name '%s'\n", conv_name);
        *failed = true;
        return nullptr;
    }
    if (conv_ensure_dir() != 0) {
        *failed = true;
        return nullptr;
    }
    return pathbuf;
}

int main(int argc, char **argv)
{
    /* `orc config ...` manages persistent settings and needs no API key. */
    if (argc >= 2 && strcmp(argv[1], "config") == 0)
        return run_config_cmd(argc, argv);

    /* Required for terminal cell-width calculations (emoji, CJK,
     * combining marks) used by the Markdown table renderer. */
    setlocale(LC_CTYPE, "");

    /* Restore the terminal if a signal interrupts alternate-screen streaming. */
    install_signal_handlers();

    Options opts;
    int exit_code = 0;
    if (!parse_args(argc, argv, &opts, &exit_code))
        return exit_code;

    char *api_key = load_api_key();
    if (!api_key) {
        fprintf(stderr,
                "error: no API key found.\n"
                "Set %s in the environment or a .env file, or run:\n"
                "  %s config set key <API_KEY>\n",
                ORC_KEY_NAME, argv[0]);
        return 1;
    }

    char namebuf[NAME_CAP];
    char pathbuf[PATH_CAP];
    bool failed = false;
    char *path = resolve_path(&opts, namebuf, sizeof namebuf,
                              pathbuf, sizeof pathbuf, &failed);
    if (failed) {
        free(api_key);
        return 1;
    }

    /* Load prior history (empty for new conversations). */
    Buffer items = {0};
    if (path && conv_load(path, &items) != 0) {
        buf_free(&items);
        free(api_key);
        return 1;
    }

    /* Resolve the model: -m flag > saved config > built-in default. */
    char *model_alloc = nullptr;
    if (!opts.model) {
        model_alloc = uconf_get("model");
        opts.model = model_alloc ? model_alloc : DEFAULT_MODEL;
    }

    OrRequest base = {
        .api_key  = api_key,
        .model    = opts.model,
        .stream   = opts.stream,
        .quiet    = 0,
        .spinner  = opts.prompt ? 0 : 1,
        .markdown = opts.markdown && !opts.prompt && md_color_enabled(),
    };

    int status;
    if (opts.prompt)
        status = run_turn(&base, &items, path, opts.prompt, nullptr, nullptr) ? 1 : 0;
    else
        status = repl(&base, &items, path, sizeof pathbuf);

    buf_free(&items);
    free(api_key);
    free(model_alloc);
    return status;
}
