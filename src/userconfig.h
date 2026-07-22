/*
 * userconfig.h — persistent per-user settings.
 *
 * Settings live in a single KEY=value file, created 0600 (directory 0700):
 *
 *   $XDG_CONFIG_HOME/orc/config   (or ~/.config/orc/config)
 *
 * This lets an installed copy of orc persist the API key and default model
 * without environment variables or a project-local .env. Writes are atomic
 * (temp file + rename) and preserve unrelated keys and comment lines.
 */
#ifndef ORC_USERCONFIG_H
#define ORC_USERCONFIG_H

/*
 * Resolved path to the user config file (malloc'd), or nullptr if neither
 * XDG_CONFIG_HOME nor HOME is set.
 */
char *uconf_path(void);

/*
 * Value for `name` from the user config file: malloc'd, trimmed and
 * unquoted, or nullptr if unset / no file / no HOME.
 */
char *uconf_get(const char *name);

/*
 * Set `name` to `value`, creating the file and its directory as needed.
 * The write is atomic and the file is 0600. Returns 0, or -1 on error
 * (a diagnostic has been printed).
 */
int uconf_set(const char *name, const char *value);

/*
 * Remove `name` from the user config file. A missing key or file is not an
 * error. Returns 0, or -1 on error (a diagnostic has been printed).
 */
int uconf_unset(const char *name);

/*
 * Read `name` from an arbitrary KEY=value file at `path`: malloc'd value
 * (trimmed/unquoted) or nullptr. Shared with the .env reader in config.c.
 */
char *kv_read(const char *path, const char *name);

#endif /* ORC_USERCONFIG_H */
