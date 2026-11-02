#ifndef ND_THREAD_H
#define ND_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct { CRITICAL_SECTION cs; } nd_mutex;
typedef struct { CONDITION_VARIABLE cv; } nd_cond;
typedef struct { HANDLE h; } nd_thread;
#else
#include <pthread.h>

typedef struct { pthread_mutex_t m; } nd_mutex;
typedef struct { pthread_cond_t c; } nd_cond;
typedef struct { pthread_t t; } nd_thread;
#endif

int  nd_mutex_init(nd_mutex *m);
void nd_mutex_destroy(nd_mutex *m);
void nd_mutex_lock(nd_mutex *m);
void nd_mutex_unlock(nd_mutex *m);

int  nd_cond_init(nd_cond *c);
void nd_cond_destroy(nd_cond *c);
void nd_cond_wait(nd_cond *c, nd_mutex *m);
/* 0 = 被唤醒，-1 = 超时 */
int  nd_cond_timedwait(nd_cond *c, nd_mutex *m, int timeout_ms);
void nd_cond_signal(nd_cond *c);
void nd_cond_broadcast(nd_cond *c);

int  nd_thread_start(nd_thread *t, void *(*fn)(void *), void *arg, size_t stack_size);
int  nd_thread_join(nd_thread *t);
void nd_thread_detach(nd_thread *t);

void nd_sleep_ms(int ms);
uint64_t nd_monotonic_ms(void);

#endif /* ND_THREAD_H */
