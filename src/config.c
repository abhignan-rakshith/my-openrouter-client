/*
 * config.c — API key discovery (.env / environment).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

char *load_api_key(void)
{
    /* Environment takes precedence over the .env file. */
    const char *env = getenv(ORC_KEY_NAME);
    if (env && *env)
        return strdup(env);

    FILE *f = fopen(ORC_ENV_FILE, "r");
    if (!f)
        return NULL;

    char line[1024];
    char *key = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, ORC_KEY_NAME "=", strlen(ORC_KEY_NAME) + 1) != 0)
            continue;
        p += strlen(ORC_KEY_NAME) + 1;

        /* Trim trailing whitespace/newline. */
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' '  || end[-1] == '\t'))
            end--;

        /* Strip one pair of surrounding quotes, if present. */
        if (end - p >= 2 && (*p == '"' || *p == '\'') && end[-1] == *p) {
            p++;
            end--;
        }
        *end = '\0';
        if (*p) {
            key = strdup(p);
            break;
        }
    }
    fclose(f);
    return key;
}
