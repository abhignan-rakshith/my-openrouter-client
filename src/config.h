/*
 * config.h — API key discovery.
 *
 * The key is looked up in this order:
 *   1. the OPENROUTER_API_KEY environment variable
 *   2. a `.env` file in the current working directory
 */
#ifndef ORC_CONFIG_H
#define ORC_CONFIG_H

#define ORC_KEY_NAME "OPENROUTER_API_KEY"
#define ORC_ENV_FILE ".env"

/*
 * Returns a malloc'd copy of the API key, or NULL if none was found.
 * Lines in .env may be `KEY=value`, optionally quoted; surrounding
 * whitespace is trimmed.
 */
char *load_api_key(void);

#endif /* ORC_CONFIG_H */
