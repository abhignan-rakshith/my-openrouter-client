/*
 * config.c — API key discovery (.env / environment).
 *
 * Lookup order (see config.h):
 *   1. the OPENROUTER_API_KEY environment variable
 *   2. a `.env` file in the current working directory
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

/* Characters treated as insignificant leading/trailing whitespace. */
static const char ORC_WS[] = " \t\r\n";

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Parse a single `.env` line in place, looking for `ORC_KEY_NAME=value`.
 *
 * On a match, returns a malloc'd copy of the trimmed, unquoted value (never
 * empty) or nullptr on allocation failure. On any non-match (blank line,
 * comment, different key, empty value) returns nullptr; use *matched to
 * distinguish "this was our key" from "keep scanning".
 *
 * `line` is mutated (trimmed/unquoted) but not freed.
 */
static char *parse_env_line(char *line, bool *matched)
{
    *matched = false;

    /* Skip leading whitespace. */
    char *p = line + strspn(line, ORC_WS);

    /* Ignore blank lines and comments. */
    if (*p == '\0' || *p == '#')
        return nullptr;

    /* Require the exact `KEY=` prefix. */
    constexpr size_t key_len = sizeof(ORC_KEY_NAME) - 1;
    if (strncmp(p, ORC_KEY_NAME, key_len) != 0 || p[key_len] != '=')
        return nullptr;

    *matched = true;
    p += key_len + 1; /* step past "KEY=" */

    /* Trim leading whitespace of the value... */
    p += strspn(p, ORC_WS);

    /* ...and trailing whitespace/newline. */
    char *end = p + strlen(p);
    while (end > p && is_ws(end[-1]))
        end--;

    /* Strip one matching pair of surrounding quotes, if present. */
    if (end - p >= 2 && (*p == '"' || *p == '\'') && end[-1] == *p) {
        p++;
        end--;
    }
    *end = '\0';

    if (*p == '\0')
        return nullptr; /* matched key but empty value */

    return strdup(p);
}

/*
 * Scan the .env file for the API key. Returns a malloc'd value, or nullptr
 * if the key is absent (or on read/alloc failure). Closes `f` before return.
 */
static char *scan_env_file(FILE *f)
{
    char *line = nullptr;
    size_t cap = 0;
    ssize_t len;
    char *key = nullptr;

    while ((len = getline(&line, &cap, f)) != -1) {
        bool matched;
        char *value = parse_env_line(line, &matched);
        if (value) {
            key = value;
            break;
        }
        if (matched)
            break; /* our key appeared with an empty value: stop looking */
    }

    free(line);
    fclose(f);
    return key;
}

char *load_api_key(void)
{
    /* Environment takes precedence over the .env file. */
    const char *env = getenv(ORC_KEY_NAME);
    if (env && *env)
        return strdup(env);

    FILE *f = fopen(ORC_ENV_FILE, "r");
    if (!f)
        return nullptr;

    return scan_env_file(f);
}
