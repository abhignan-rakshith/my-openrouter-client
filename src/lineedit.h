/*
 * lineedit.h — raw-mode interactive line editor.
 *
 * A small readline-style editor for the chat prompt: UTF-8 aware
 * cursor movement and editing, horizontal scrolling for long lines,
 * and a hook on Ctrl+V so the caller can pull an image off the OS
 * clipboard and splice a placeholder like "[Image 1]" into the line.
 *
 * When stdin is not a terminal the editor transparently falls back to
 * getline(), so piped input keeps working.
 */
#ifndef ORC_LINEEDIT_H
#define ORC_LINEEDIT_H

/*
 * Ctrl+V callback. May set *msg to a malloc'd status/error line for the
 * editor to print above the prompt (the editor frees it). Returns a
 * malloc'd string to insert at the cursor (the editor frees it), or
 * NULL to insert nothing.
 */
typedef char *(*LePasteCb)(void *user, char **msg);

/*
 * Read one line, editing in raw mode when stdin is a terminal. The
 * prompt may contain ANSI color escapes (excluded from width math).
 * Returns a malloc'd line without the trailing newline, or NULL on
 * EOF (Ctrl-D on an empty line, or end of piped input).
 */
char *le_readline(const char *prompt, LePasteCb on_paste, void *paste_user);

/*
 * Async-signal-safe: restore the terminal's original attributes if raw
 * mode is active. For use in fatal-signal handlers so the terminal is
 * never left in raw mode.
 */
void le_signal_restore(void);

#endif /* ORC_LINEEDIT_H */
