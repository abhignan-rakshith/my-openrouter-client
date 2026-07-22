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
 * Returns a malloc'd copy of the API key, or NULL if none was found;
 * the caller owns the returned buffer and must free() it.
 *
 * The environment variable takes precedence over the .env file. In .env,
 * the key is read from a `KEY=value` line: surrounding whitespace is
 * trimmed and one matching pair of surrounding quotes ('"' or '\'') is
 * stripped. Blank lines, comments (`#`) and other keys are ignored, and
 * an empty value counts as "not found".
 */
char *load_api_key(void);

#endif /* ORC_CONFIG_H */
