/* plat.c -- see plat.h.
 *
 * SPDX-License-Identifier: MIT
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>

#include "plat.h"

#ifdef _WIN32

uint64_t mb8_now_ms(void) { return (uint64_t)GetTickCount64(); }
void mb8_sleep_ms(unsigned ms) { Sleep(ms); }

void mb8_mutex_init(mb8_mutex *m) { InitializeCriticalSection(m); }
void mb8_mutex_destroy(mb8_mutex *m) { DeleteCriticalSection(m); }
void mb8_mutex_lock(mb8_mutex *m) { EnterCriticalSection(m); }
void mb8_mutex_unlock(mb8_mutex *m) { LeaveCriticalSection(m); }

void mb8_cond_init(mb8_cond *c) { InitializeConditionVariable(c); }
void mb8_cond_destroy(mb8_cond *c) { (void)c; }
void mb8_cond_broadcast(mb8_cond *c) { WakeAllConditionVariable(c); }
void mb8_cond_wait_ms(mb8_cond *c, mb8_mutex *m, unsigned ms)
{
    (void)SleepConditionVariableCS(c, m, ms);
}

struct start { mb8_thread_fn fn; void *arg; };

static DWORD WINAPI trampoline(LPVOID p)
{
    struct start s = *(struct start *)p;

    free(p);
    s.fn(s.arg);
    return 0;
}

int mb8_thread_start(mb8_thread *t, mb8_thread_fn fn, void *arg)
{
    struct start *s = malloc(sizeof(*s));

    if (s == NULL)
        return -1;
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, trampoline, s, 0, NULL);
    if (*t == NULL) {
        free(s);
        return -1;
    }
    return 0;
}

void mb8_thread_join(mb8_thread t)
{
    (void)WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else  /* POSIX */

#include <errno.h>
#include <time.h>

uint64_t mb8_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void mb8_sleep_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

void mb8_mutex_init(mb8_mutex *m) { pthread_mutex_init(m, NULL); }
void mb8_mutex_destroy(mb8_mutex *m) { pthread_mutex_destroy(m); }
void mb8_mutex_lock(mb8_mutex *m) { pthread_mutex_lock(m); }
void mb8_mutex_unlock(mb8_mutex *m) { pthread_mutex_unlock(m); }

/* CLOCK_REALTIME for the deadline: pthread_condattr_setclock is not on
 * macOS, and a wait that ends early or late by a clock step only costs a
 * loop in the caller. */
void mb8_cond_init(mb8_cond *c) { pthread_cond_init(c, NULL); }
void mb8_cond_destroy(mb8_cond *c) { pthread_cond_destroy(c); }
void mb8_cond_broadcast(mb8_cond *c) { pthread_cond_broadcast(c); }

void mb8_cond_wait_ms(mb8_cond *c, mb8_mutex *m, unsigned ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    (void)pthread_cond_timedwait(c, m, &ts);
}

struct start { mb8_thread_fn fn; void *arg; };

static void *trampoline(void *p)
{
    struct start s = *(struct start *)p;

    free(p);
    s.fn(s.arg);
    return NULL;
}

int mb8_thread_start(mb8_thread *t, mb8_thread_fn fn, void *arg)
{
    struct start *s = malloc(sizeof(*s));

    if (s == NULL)
        return -1;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, trampoline, s) != 0) {
        free(s);
        return -1;
    }
    return 0;
}

void mb8_thread_join(mb8_thread t) { (void)pthread_join(t, NULL); }

#endif
