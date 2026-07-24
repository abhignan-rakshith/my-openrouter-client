/*
 * screen.c — alternate-screen control. See screen.h for the contract.
 */
#include <stdio.h>

#include "screen.h"

volatile sig_atomic_t g_in_alt_screen = 0;

void screen_alt_enter(void)
{
    fputs("\033[?1049h\033[H", stdout);
    fflush(stdout);
    g_in_alt_screen = 1;        /* mark only once the enter is on the wire */
}

void screen_alt_leave(void)
{
    fputs("\033[?1049l", stdout);
    fflush(stdout);
    g_in_alt_screen = 0;        /* clear only after the leave is on the wire */
}
