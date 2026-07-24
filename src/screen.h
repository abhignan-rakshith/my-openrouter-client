/*
 * screen.h — alternate-screen control shared across the app.
 *
 * The alternate screen buffer has no scrollback, so anything drawn on it
 * disappears on exit and the original screen is restored. It backs the
 * live streaming re-render, the /help page, and the model picker. The
 * g_in_alt_screen flag lets a fatal-signal handler restore the main
 * screen before the process dies, so a signal mid-draw never strands the
 * terminal on the alternate buffer.
 */
#ifndef ORC_SCREEN_H
#define ORC_SCREEN_H

#include <signal.h>

/* Nonzero while the alternate screen is active. sig_atomic_t so a signal
 * handler may read it safely. Set/cleared only once the enter/leave
 * escape is actually on the wire. */
extern volatile sig_atomic_t g_in_alt_screen;

/* Enter / leave the terminal's alternate screen buffer. */
void screen_alt_enter(void);
void screen_alt_leave(void);

#endif /* ORC_SCREEN_H */
