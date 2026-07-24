/*
 * save.h — export a single reply to a Markdown file.
 *
 * The interactive /save command writes the most recent assistant reply,
 * verbatim, to saves/<name>.md. The reply is already plain Markdown text
 * (no ANSI styling, no JSON escaping), so the saved file is clean and
 * ready to read or share.
 */
#ifndef ORC_SAVE_H
#define ORC_SAVE_H

#include <stddef.h>

#define ORC_SAVE_DIR "saves"

/*
 * Build the on-disk path for a save name into out (a buffer of outsz
 * bytes). ".md" is appended unless already present. Returns 0, or -1 if
 * the name is empty, contains '/', or overflows.
 */
int save_path(const char *name, char *out, size_t outsz);

/* Create the saves directory if needed. 0 on success, -1 on error
 * (a diagnostic is printed). */
int save_ensure_dir(void);

/*
 * Write `content` to `path`, overwriting any existing file, and ensure it
 * ends with a single trailing newline. Returns 0 on success, -1 on error
 * (a diagnostic is printed).
 */
int save_write(const char *path, const char *content);

#endif /* ORC_SAVE_H */
