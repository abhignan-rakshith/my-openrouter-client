/*
 * config.c — API key discovery.
 *
 * Lookup order (see config.h):
 *   1. the OPENROUTER_API_KEY environment variable
 *   2. a `.env` file in the current working directory
 *   3. the persistent user config (~/.config/orc/config)
 *
 * The KEY=value parsing itself lives in userconfig.c (kv_read), shared with
 * the user config file.
 */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "userconfig.h"

char *load_api_key(void)
{
    /* Environment takes precedence over any file. */
    const char *env = getenv(ORC_KEY_NAME);
    if (env && *env)
        return strdup(env);

    /* Then a project-local .env, then the persistent user config. */
    char *v = kv_read(ORC_ENV_FILE, ORC_KEY_NAME);
    if (v)
        return v;

    return uconf_get(ORC_KEY_NAME);
}
