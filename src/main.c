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
 *   /rename NAME  rename the current conversation
 *   /quit         end the session
 *
 * Conversations are stored in conversations/<name>.jsonl, one
 * {"role":...,"content":...} object per line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <unistd.h>

#include "api.h"
#include "buffer.h"
#include "config.h"
#include "conv.h"
#include "md.h"
#include "userconfig.h"

#define DEFAULT_MODEL "google/gemini-3.6-flash"

/* Longest conversation on-disk path we build (matches conv module scope). */
constexpr size_t PATH_CAP = 512;
/* Longest auto-generated conversation name (chat-YYYYMMDD-HHMMSS + slack). */
constexpr size_t NAME_CAP = 64;

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
        "In the chat, /rename NAME renames the conversation.\n"
        "/quit (or Ctrl-D) ends the session.\n"
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

/* Print the interactive "you>" prompt. */
static void print_you_prompt(bool markdown)
{
    if (markdown && md_color_enabled())
        fputs("\n\033[1;38;5;84myou>\033[0m ", stdout);
    else
        fputs("\nyou> ", stdout);
    fflush(stdout);
}

/* Enter / leave the terminal's alternate screen buffer. The alternate
 * buffer has no scrollback, so text streamed into it disappears completely
 * on exit and the original screen is restored — no matter how long the
 * reply was. This is how the live plain stream is shown and then discarded
 * before the formatted reply is drawn on the main screen. */
/* Set while the alternate screen is active, so a fatal signal can restore
 * the main screen before the process dies. sig_atomic_t for handler safety. */
static volatile sig_atomic_t g_in_alt_screen = 0;

static void alt_screen_enter(void)
{
    fputs("\033[?1049h\033[H", stdout);
    fflush(stdout);
    g_in_alt_screen = 1;        /* mark only once the enter is on the wire */
}

static void alt_screen_leave(void)
{
    fputs("\033[?1049l", stdout);
    fflush(stdout);
    g_in_alt_screen = 0;        /* clear only after the leave is on the wire */
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
 * doesn't thrash the terminal; the cursor is homed before each frame and
 * the area below it cleared, so every repaint overwrites the last in place.
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

    fputs("\033[H", stdout);            /* home to top-left of alt screen */
    print_assistant_banner();
    md_render(reply, md_color_enabled());
    fputs("\033[0J", stdout);           /* clear any taller previous frame */
    fflush(stdout);
}

/*
 * Run one exchange: append the user message to the in-memory history
 * (and file, if any), call the API with the full history, then record
 * the assistant's reply. Returns 0 on success, -1 on failure.
 *
 * In an interactive color terminal the reply streams in as plain text on
 * the alternate screen for responsiveness; once complete that view is
 * dropped and the finished reply is re-drawn with Markdown styling on the
 * main screen, so only the formatted copy remains.
 */
static int run_turn(const OrRequest *base, Buffer *items,
                    const char *path, const char *user_msg)
{
    if (conv_items_add(items, "user", user_msg) != 0) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }
    if (path && conv_append(path, "user", user_msg) != 0)
        return -1;

    /* Wrap the accumulated items in a JSON array. */
    Buffer msgs = {0};
    if (buf_append_str(&msgs, "[")         ||
        buf_append_str(&msgs, items->data) ||
        buf_append_str(&msgs, "]")) {
        fprintf(stderr, "error: out of memory\n");
        buf_free(&msgs);
        return -1;
    }

    OrRequest req = *base;
    req.messages_json = msgs.data;

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
    if (live) {
        /* Stream into the alternate screen, rendered live as Markdown; a
         * transient banner heads it until the first repaint. */
        alt_screen_enter();
        print_assistant_banner();
        fflush(stdout);
        req.on_update = stream_repaint;
        req.on_update_user = &paint;
    }

    char *reply = nullptr;
    int rc = or_chat(&req, &reply);

    if (live)               /* drop the raw stream; back to the main screen */
        alt_screen_leave();

    buf_free(&msgs);
    if (rc != 0)
        return -1;

    if (render) {
        print_assistant_banner();
        md_render(reply, md_color_enabled());
    }

    rc = 0;
    if (conv_items_add(items, "assistant", reply) != 0) {
        fprintf(stderr, "error: out of memory\n");
        rc = -1;
    } else if (path && conv_append(path, "assistant", reply) != 0) {
        rc = -1;
    }
    free(reply);
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

/* Print the session header, and prior history when resuming. */
static int repl_intro(const OrRequest *base, const Buffer *items,
                      const char *path)
{
    printf("Chatting with %s — /quit or Ctrl-D to exit.\n", base->model);
    printf("Markdown: %s\n", base->markdown
           ? "on (rendered live as it streams)" : "off");
    if (path) {
        printf("Conversation: %s\n", path);
        if (items->len) {
            printf("\n--- Previous messages ---\n");
            if (conv_show(path, base->markdown) != 0)
                return 1;
            printf("\n--- Resuming chat ---\n");
        }
    }
    return 0;
}

/* Interactive read–send–print loop. */
static int repl(OrRequest *base, Buffer *items, char *path, size_t pathsz)
{
    if (repl_intro(base, items, path) != 0)
        return 1;

    char *line = nullptr;
    size_t cap = 0;
    int status = 0;
    for (;;) {
        print_you_prompt(base->markdown);

        ssize_t n = getline(&line, &cap, stdin);
        if (n == -1) { /* EOF */
            putchar('\n');
            break;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0)
            continue;

        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0)
            break;
        if (is_command(line, "/rename", 7)) {
            handle_rename(line + 7, path, pathsz);
            continue;
        }

        putchar('\n');
        if (run_turn(base, items, path, line) != 0) {
            /* Report and keep the session alive; the user may retry. */
            status = 1;
        }
    }
    free(line);
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
        status = run_turn(&base, &items, path, opts.prompt) ? 1 : 0;
    else
        status = repl(&base, &items, path, sizeof pathbuf);

    buf_free(&items);
    free(api_key);
    free(model_alloc);
    return status;
}
