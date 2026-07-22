/*
 * spinner.c — terminal "thinking" spinner.
 *
 * A background thread animates a Braille spinner on stderr while a blocking
 * call is in flight. Synchronization uses a mutex + condition variable: the
 * animation thread waits on the condvar between frames, so spinner_stop wakes
 * it immediately instead of blocking on a full frame interval. All access to
 * the `running` flag is under the mutex, so there is no data race.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "spinner.h"

/* Animation frames and the delay between them. */
static const char *const kFrames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};
constexpr size_t kFrameCount = sizeof kFrames / sizeof kFrames[0];
constexpr long kFrameNanos = 80'000'000L; /* 80 ms */

/* ANSI: carriage return + erase entire line. */
#define ANSI_CLEAR_LINE "\r\033[2K"

struct Spinner {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool running;
    char *label;
};

/*
 * Compute an absolute deadline `nanos` in the future, using CLOCK_REALTIME to
 * match pthread_cond_timedwait's default clock.
 */
static struct timespec deadline_after(long nanos)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    ts.tv_nsec += nanos;
    ts.tv_sec += ts.tv_nsec / 1'000'000'000L;
    ts.tv_nsec %= 1'000'000'000L;
    return ts;
}

static void *spin(void *arg)
{
    Spinner *s = arg;
    size_t frame = 0;

    pthread_mutex_lock(&s->mutex);
    while (s->running) {
        fprintf(stderr, ANSI_CLEAR_LINE "%s %s…",
                kFrames[frame++ % kFrameCount], s->label);
        fflush(stderr);

        /*
         * Sleep until the next frame, but wake early if stop signals us.
         * Spurious wakeups just redraw the same frame — harmless.
         */
        struct timespec until = deadline_after(kFrameNanos);
        pthread_cond_timedwait(&s->cond, &s->mutex, &until);
    }
    pthread_mutex_unlock(&s->mutex);
    return nullptr;
}

Spinner *spinner_start(const char *label)
{
    if (!isatty(STDERR_FILENO))
        return nullptr;

    Spinner *s = calloc(1, sizeof *s);
    if (!s)
        return nullptr;

    s->label = strdup(label ? label : "thinking");
    if (!s->label)
        goto fail_label;

    if (pthread_mutex_init(&s->mutex, nullptr) != 0)
        goto fail_mutex;
    if (pthread_cond_init(&s->cond, nullptr) != 0)
        goto fail_cond;

    s->running = true;
    if (pthread_create(&s->thread, nullptr, spin, s) != 0)
        goto fail_thread;

    return s;

fail_thread:
    pthread_cond_destroy(&s->cond);
fail_cond:
    pthread_mutex_destroy(&s->mutex);
fail_mutex:
    free(s->label);
fail_label:
    free(s);
    return nullptr;
}

void spinner_stop(Spinner *s)
{
    if (!s)
        return;

    /* Signal stop and wake the animation thread promptly. */
    pthread_mutex_lock(&s->mutex);
    s->running = false;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    pthread_join(s->thread, nullptr);

    fputs(ANSI_CLEAR_LINE, stderr);
    fflush(stderr);

    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->mutex);
    free(s->label);
    free(s);
}
