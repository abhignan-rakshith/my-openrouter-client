/*
 * conv.h — JSONL conversation persistence.
 *
 * Conversations live under conversations/<name>.jsonl, one message
 * per line in the standard chat format:
 *
 *   {"role":"user","content":"..."}
 *   {"role":"assistant","content":"..."}
 *
 * Because each line is itself a valid `messages` array element, the
 * request payload is rebuilt by simply joining lines with commas.
 */
#ifndef ORC_CONV_H
#define ORC_CONV_H

#include "buffer.h"

#define ORC_CONV_DIR "conversations"

/*
 * Build the on-disk path for a conversation name into out (a buffer
 * of outsz bytes). ".jsonl" is appended unless already present.
 * Returns 0, or -1 if the name is empty, contains '/', or overflows.
 */
int conv_path(const char *name, char *out, size_t outsz);

/* Create the conversations directory if needed. 0 on success. */
int conv_ensure_dir(void);

/*
 * Load an existing conversation into `items` as comma-joined JSON
 * objects (no surrounding brackets). A missing file is not an error —
 * the conversation simply starts empty. Malformed lines are skipped
 * with a warning. Returns 0 on success, -1 on I/O or memory failure.
 */
int conv_load(const char *path, Buffer *items);

/*
 * Append one message line to the conversation file, creating it if
 * necessary. The content is JSON-escaped here. Returns 0 on success.
 */
int conv_append(const char *path, const char *role, const char *content);

/*
 * Append a message to an in-memory items buffer (same encoding as
 * conv_load produces). Returns 0 on success.
 */
int conv_items_add(Buffer *items, const char *role, const char *content);

/*
 * Print an existing conversation to stdout in the same visual style as
 * the interactive prompt, so a resumed session shows its history.
 * A missing file is not an error. Returns 0 on success.
 */
int conv_show(const char *path, int markdown);

/*
 * Rename a conversation file from oldpath to newpath. If oldpath does
 * not exist yet (no messages written), this succeeds so the caller can
 * simply adopt newpath. Refuses to overwrite an existing newpath.
 * Returns 0 on success, -1 on error (a diagnostic is printed).
 */
int conv_rename(const char *oldpath, const char *newpath);

#endif /* ORC_CONV_H */
