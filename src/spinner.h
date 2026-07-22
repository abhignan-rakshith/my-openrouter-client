/*
 * spinner.h — a tiny background "thinking" spinner.
 *
 * The spinner animates on stderr in its own thread so it can run while
 * a blocking network call is in flight. It is a no-op when stderr is
 * not a terminal, so redirected/piped output stays clean. Stopping it
 * erases the spinner line, leaving the cursor ready for real output.
 */
#ifndef ORC_SPINNER_H
#define ORC_SPINNER_H

typedef struct Spinner Spinner;

/*
 * Start a spinner with the given label (e.g. "thinking"). Returns a
 * handle to pass to spinner_stop, or NULL if no spinner was started
 * (not a TTY, or allocation/thread failure). NULL is safe to pass to
 * spinner_stop.
 */
Spinner *spinner_start(const char *label);

/* Stop the spinner, erase its line, and release resources. */
void spinner_stop(Spinner *s);

#endif /* ORC_SPINNER_H */
