/*
 * spinner.c — terminal thinking spinner.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "spinner.h"

struct Spinner {
    pthread_t thread;
    atomic_int running;
    char *label;
};

static void *spin(void *arg)
{
    Spinner *s = arg;
    static const char *frames[] = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
    };
    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 80000000L
    };
    size_t frame = 0;

    while (atomic_load(&s->running)) {
        fprintf(stderr, "\r\033[2K%s %s…",
                frames[frame++ % (sizeof frames / sizeof frames[0])],
                s->label);
        fflush(stderr);
        nanosleep(&delay, NULL);
    }
    return NULL;
}

Spinner *spinner_start(const char *label)
{
    if (!isatty(STDERR_FILENO))
        return NULL;

    Spinner *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->label = strdup(label ? label : "thinking");
    if (!s->label) {
        free(s);
        return NULL;
    }

    atomic_init(&s->running, 1);
    if (pthread_create(&s->thread, NULL, spin, s) != 0) {
        free(s->label);
        free(s);
        return NULL;
    }
    return s;
}

void spinner_stop(Spinner *s)
{
    if (!s)
        return;
    atomic_store(&s->running, 0);
    pthread_join(s->thread, NULL);
    fputs("\r\033[2K", stderr);
    fflush(stderr);
    free(s->label);
    free(s);
}
