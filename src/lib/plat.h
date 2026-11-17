/* plat.h -- the few things that differ between POSIX and Windows: time,
 * sleeping, threads, a mutex and a condition variable.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MB8_PLAT_H
#define MB8_PLAT_H

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef CRITICAL_SECTION   mb8_mutex;
typedef CONDITION_VARIABLE mb8_cond;
typedef HANDLE             mb8_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t    mb8_mutex;
typedef pthread_cond_t     mb8_cond;
typedef pthread_t          mb8_thread;
#endif

uint64_t mb8_now_ms(void);              /* monotonic                    */
void     mb8_sleep_ms(unsigned ms);

void mb8_mutex_init(mb8_mutex *m);
void mb8_mutex_destroy(mb8_mutex *m);
void mb8_mutex_lock(mb8_mutex *m);
void mb8_mutex_unlock(mb8_mutex *m);

void mb8_cond_init(mb8_cond *c);
void mb8_cond_destroy(mb8_cond *c);
void mb8_cond_broadcast(mb8_cond *c);
/* Wait with `m` held, at most `ms`.  Spurious wakeups are the caller's. */
void mb8_cond_wait_ms(mb8_cond *c, mb8_mutex *m, unsigned ms);

typedef void (*mb8_thread_fn)(void *arg);
int  mb8_thread_start(mb8_thread *t, mb8_thread_fn fn, void *arg);
void mb8_thread_join(mb8_thread t);

#endif
