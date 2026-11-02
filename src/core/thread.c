#include "core/thread.h"
#include "core/log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32

#include <stdlib.h>

int nd_mutex_init(nd_mutex *m)
{
    InitializeCriticalSection(&m->cs);
    return 0;
}
void nd_mutex_destroy(nd_mutex *m) { DeleteCriticalSection(&m->cs); }
void nd_mutex_lock(nd_mutex *m) { EnterCriticalSection(&m->cs); }
void nd_mutex_unlock(nd_mutex *m) { LeaveCriticalSection(&m->cs); }

int nd_cond_init(nd_cond *c)
{
    InitializeConditionVariable(&c->cv);
    return 0;
}
void nd_cond_destroy(nd_cond *c) { (void)c; }
void nd_cond_wait(nd_cond *c, nd_mutex *m)
{
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
}
int nd_cond_timedwait(nd_cond *c, nd_mutex *m, int timeout_ms)
{
    if (timeout_ms < 0)
        timeout_ms = 0;
    return SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)timeout_ms) ? 0 : -1;
}
void nd_cond_signal(nd_cond *c) { WakeConditionVariable(&c->cv); }
void nd_cond_broadcast(nd_cond *c) { WakeAllConditionVariable(&c->cv); }

typedef struct {
    void *(*fn)(void *);
    void *arg;
} thread_boot;

static DWORD WINAPI thread_tramp(LPVOID p)
{
    thread_boot b = *(thread_boot *)p;
    free(p);
    b.fn(b.arg);
    return 0;
}

int nd_thread_start(nd_thread *t, void *(*fn)(void *), void *arg, size_t stack_size)
{
    thread_boot *b = (thread_boot *)malloc(sizeof(*b));
    if (!b)
        return -1;
    b->fn = fn;
    b->arg = arg;
    t->h = CreateThread(NULL, (SIZE_T)stack_size, thread_tramp, b, 0, NULL);
    if (!t->h) {
        free(b);
        return -1;
    }
    return 0;
}
int nd_thread_join(nd_thread *t)
{
    DWORD rc;
    if (!t->h)
        return 0;
    rc = WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    t->h = NULL;
    return rc == WAIT_OBJECT_0 ? 0 : -1;
}
void nd_thread_detach(nd_thread *t)
{
    if (t->h) {
        CloseHandle(t->h);
        t->h = NULL;
    }
}
void nd_sleep_ms(int ms) { Sleep((DWORD)ms); }

static uint64_t win_mhz = 0;
uint64_t nd_monotonic_ms(void)
{
    LARGE_INTEGER f, c;
    if (!win_mhz) {
        QueryPerformanceFrequency(&f);
        win_mhz = (uint64_t)f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000ULL) / win_mhz);
}

#else /* POSIX */

#include <errno.h>
#include <stdlib.h>

int nd_mutex_init(nd_mutex *m) { return pthread_mutex_init(&m->m, NULL); }
void nd_mutex_destroy(nd_mutex *m) { pthread_mutex_destroy(&m->m); }
void nd_mutex_lock(nd_mutex *m) { pthread_mutex_lock(&m->m); }
void nd_mutex_unlock(nd_mutex *m) { pthread_mutex_unlock(&m->m); }

int nd_cond_init(nd_cond *c)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(&c->c, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}
void nd_cond_destroy(nd_cond *c) { pthread_cond_destroy(&c->c); }
void nd_cond_wait(nd_cond *c, nd_mutex *m) { pthread_cond_wait(&c->c, &m->m); }

int nd_cond_timedwait(nd_cond *c, nd_mutex *m, int timeout_ms)
{
    struct timespec ts;
    uint64_t deadline = nd_monotonic_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    ts.tv_sec = (time_t)(deadline / 1000);
    ts.tv_nsec = (long)((deadline % 1000) * 1000000ULL);
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == ETIMEDOUT ? -1 : 0;
}
void nd_cond_signal(nd_cond *c) { pthread_cond_signal(&c->c); }
void nd_cond_broadcast(nd_cond *c) { pthread_cond_broadcast(&c->c); }

typedef struct {
    void *(*fn)(void *);
    void *arg;
} thread_boot;

static void *thread_tramp(void *p)
{
    thread_boot b = *(thread_boot *)p;
    free(p);
    return b.fn(b.arg);
}

int nd_thread_start(nd_thread *t, void *(*fn)(void *), void *arg, size_t stack_size)
{
    pthread_attr_t attr;
    thread_boot *b = (thread_boot *)malloc(sizeof(*b));
    if (!b)
        return -1;
    b->fn = fn;
    b->arg = arg;
    pthread_attr_init(&attr);
    if (stack_size)
        pthread_attr_setstacksize(&attr, stack_size < 65536 ? 65536 : stack_size);
    int rc = pthread_create(&t->t, &attr, thread_tramp, b);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(b);
        return -1;
    }
    return 0;
}
int nd_thread_join(nd_thread *t)
{
    if (!t->t)
        return 0;
    int rc = pthread_join(t->t, NULL);
    t->t = 0;
    return rc == 0 ? 0 : -1;
}
void nd_thread_detach(nd_thread *t)
{
    if (t->t) {
        pthread_detach(t->t);
        t->t = 0;
    }
}
void nd_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

uint64_t nd_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

#endif
