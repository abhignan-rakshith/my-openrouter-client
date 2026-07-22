/*
 * main.c — orc, a minimal OpenRouter chat CLI in native C.
 *
 * Modes:
 *   orc "prompt"              one-shot question (not persisted)
 *   orc                       interactive chat, auto-named conversation
 *   orc -c NAME               resume (or start) conversation NAME
 *   orc -c NAME "prompt"      one turn appended to conversation NAME
 *
 * Options:
 *   -m MODEL      model id (default: DEFAULT_MODEL)
 *   --no-stream   wait for the full response instead of streaming
 *   --no-markdown disable colored Markdown rendering
 *
 * Interactive commands:
 *   /rename NAME  rename the current conversation
 *   /markdown     toggle Markdown rendering
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

#include "api.h"
#include "buffer.h"
#include "config.h"
#include "conv.h"
#include "md.h"

#define DEFAULT_MODEL "google/gemini-3.6-flash"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-m model] [-c name] [--no-stream] [--no-markdown]"
        " [\"prompt\"]\n"
        "  -m model     model id (default: %s)\n"
        "  -c name      persist to conversations/<name>.jsonl\n"
        "  --no-stream  wait for the full response instead of streaming\n"
        "  --no-markdown  disable colored Markdown in the interactive REPL\n"
        "\n"
        "With no prompt, an interactive chat session starts.\n"
        "In the chat, /rename NAME renames the conversation.\n"
        "/markdown [on|off] toggles Markdown rendering.\n"
        "/quit (or Ctrl-D) ends the session.\n",
        prog, DEFAULT_MODEL);
}

/*
 * Run one exchange: append the user message to the in-memory history
 * (and file, if any), call the API with the full history, then record
 * the assistant's reply. Returns 0 on success.
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
    if (buf_append_str(&msgs, "[")          ||
        buf_append_str(&msgs, items->data)  ||
        buf_append_str(&msgs, "]")) {
        fprintf(stderr, "error: out of memory\n");
        buf_free(&msgs);
        return -1;
    }

    OrRequest req = *base;
    req.messages_json = msgs.data;
    if (req.markdown)
        req.quiet = 1; /* render the complete Markdown reply below */

    char *reply = NULL;
    int rc = or_chat(&req, &reply);
    buf_free(&msgs);
    if (rc != 0)
        return -1;

    if (req.markdown) {
        if (md_color_enabled())
            fputs("\033[1;38;5;141massistant>\033[0m\n", stdout);
        else
            fputs("assistant>\n", stdout);
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

/* Interactive read–send–print loop. */
static int repl(OrRequest *base, Buffer *items,
                char *path, size_t pathsz)
{
    printf("Chatting with %s — /quit or Ctrl-D to exit.\n", base->model);
    printf("Markdown: %s%s\n", base->markdown ? "on" : "off",
           base->markdown ? " (responses render after completion)" : "");
    if (path) {
        printf("Conversation: %s\n", path);
        if (items->len) {
            printf("\n--- Previous messages ---\n");
            if (conv_show(path, base->markdown) != 0)
                return 1;
            printf("\n--- Resuming chat ---\n");
        }
    }

    char *line = NULL;
    size_t cap = 0;
    int status = 0;
    for (;;) {
        if (base->markdown && md_color_enabled())
            fputs("\n\033[1;38;5;84myou>\033[0m ", stdout);
        else
            fputs("\nyou> ", stdout);
        fflush(stdout);
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
        if (strncmp(line, "/rename", 7) == 0 &&
            (line[7] == '\0' || line[7] == ' ' || line[7] == '\t')) {
            char *name = line + 7;
            while (*name == ' ' || *name == '\t')
                name++;
            if (!*name) {
                fprintf(stderr, "usage: /rename NAME\n");
                continue;
            }
            char newpath[512];
            if (!path || conv_path(name, newpath, sizeof newpath) != 0) {
                fprintf(stderr,
                        "error: invalid conversation name '%s'\n", name);
                continue;
            }
            if (strlen(newpath) + 1 > pathsz) {
                fprintf(stderr, "error: conversation name is too long\n");
                continue;
            }
            if (conv_rename(path, newpath) != 0)
                continue;
            strcpy(path, newpath);
            printf("Conversation renamed to %s\n", path);
            continue;
        }
        if (strncmp(line, "/markdown", 9) == 0 &&
            (line[9] == '\0' || line[9] == ' ' || line[9] == '\t')) {
            char *value = line + 9;
            while (*value == ' ' || *value == '\t')
                value++;

            int enabled;
            if (!*value)
                enabled = !base->markdown;
            else if (strcmp(value, "on") == 0)
                enabled = 1;
            else if (strcmp(value, "off") == 0)
                enabled = 0;
            else {
                fprintf(stderr, "usage: /markdown [on|off]\n");
                continue;
            }

            if (enabled && !md_stdout_is_tty()) {
                fprintf(stderr,
                        "error: Markdown rendering requires a terminal\n");
                continue;
            }
            base->markdown = enabled;
            printf("Markdown rendering %s%s.\n",
                   enabled ? "enabled" : "disabled",
                   enabled ? " (responses will be buffered)" : "");
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
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, outsz, "chat-%Y%m%d-%H%M%S", &tm);
}

int main(int argc, char **argv)
{
    /* Required for terminal cell-width calculations (emoji, CJK,
     * combining marks) used by the Markdown table renderer. */
    setlocale(LC_CTYPE, "");

    const char *model = DEFAULT_MODEL;
    const char *prompt = NULL;
    const char *conv_name = NULL;
    int stream = 1;
    int markdown = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            conv_name = argv[++i];
        } else if (strcmp(argv[i], "--no-stream") == 0) {
            stream = 0;
        } else if (strcmp(argv[i], "--no-markdown") == 0) {
            markdown = 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (!prompt) {
            prompt = argv[i];
        } else {
            fprintf(stderr, "error: multiple prompts given\n");
            usage(argv[0]);
            return 1;
        }
    }

    char *api_key = load_api_key();
    if (!api_key) {
        fprintf(stderr,
                "error: no API key found.\n"
                "Set %s in the environment or in %s\n",
                ORC_KEY_NAME, ORC_ENV_FILE);
        return 1;
    }

    /* Resolve the conversation file, if persistence is in play.
     * Interactive sessions are always persisted (auto-named when the
     * user didn't pick a name); bare one-shots stay ephemeral. */
    char namebuf[64];
    char pathbuf[512];
    char *path = NULL;
    if (!conv_name && !prompt) {
        auto_name(namebuf, sizeof namebuf);
        conv_name = namebuf;
    }
    if (conv_name) {
        if (conv_path(conv_name, pathbuf, sizeof pathbuf) != 0) {
            fprintf(stderr,
                    "error: invalid conversation name '%s'\n", conv_name);
            free(api_key);
            return 1;
        }
        if (conv_ensure_dir() != 0) {
            free(api_key);
            return 1;
        }
        path = pathbuf;
    }

    /* Load prior history (empty for new conversations). */
    Buffer items = {0};
    if (path && conv_load(path, &items) != 0) {
        buf_free(&items);
        free(api_key);
        return 1;
    }

    OrRequest base = {
        .api_key = api_key,
        .model   = model,
        .stream  = stream,
        .quiet   = 0,
        .spinner = prompt ? 0 : 1,
        .markdown = markdown && !prompt && md_stdout_is_tty(),
    };

    int status;
    if (prompt)
        status = run_turn(&base, &items, path, prompt) ? 1 : 0;
    else
        status = repl(&base, &items, path, sizeof pathbuf);

    buf_free(&items);
    free(api_key);
    return status;
}
