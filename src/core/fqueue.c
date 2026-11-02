#include "core/fqueue.h"

#include <stdlib.h>
#include <string.h>

int nd_fqueue_init(nd_fqueue *q, size_t limit)
{
    memset(q, 0, sizeof(*q));
    if (nd_mutex_init(&q->lock) != 0)
        return -1;
    if (nd_cond_init(&q->cond) != 0) {
        nd_mutex_destroy(&q->lock);
        return -1;
    }
    q->limit = limit ? limit : (size_t)1 << 20;
    return 0;
}

void nd_fqueue_destroy(nd_fqueue *q)
{
    nd_frame *f = q->head;
    while (f) {
        nd_frame *next = f->next;
        free(f);
        f = next;
    }
    q->head = q->tail = NULL;
    q->bytes = 0;
    nd_cond_destroy(&q->cond);
    nd_mutex_destroy(&q->lock);
}

int nd_fqueue_push(nd_fqueue *q, const void *data, size_t len, int timeout_ms)
{
    nd_frame *f;
    uint64_t deadline = 0;
    int infinite = timeout_ms < 0;

    if (len == 0)
        return 0;
    if (!infinite)
        deadline = nd_monotonic_ms() + (uint64_t)timeout_ms;

    f = (nd_frame *)malloc(sizeof(nd_frame) + len);
    if (!f)
        return -1;
    f->next = NULL;
    f->len = len;
    memcpy(f->data, data, len);

    nd_mutex_lock(&q->lock);
    while (!q->closed && q->bytes > 0 && q->bytes + len > q->limit) {
        int rc;
        if (infinite) {
            rc = nd_cond_timedwait(&q->cond, &q->lock, 1000);
            (void)rc;
            continue;
        }
        {
            uint64_t now = nd_monotonic_ms();
            if (now >= deadline) {
                q->dropped++;
                nd_mutex_unlock(&q->lock);
                free(f);
                return -1;
            }
            nd_cond_timedwait(&q->cond, &q->lock, (int)(deadline - now));
        }
    }
    if (q->closed) {
        nd_mutex_unlock(&q->lock);
        free(f);
        return -2;
    }
    if (q->tail)
        q->tail->next = f;
    else
        q->head = f;
    q->tail = f;
    q->bytes += len;
    nd_cond_signal(&q->cond);
    nd_mutex_unlock(&q->lock);
    return 0;
}

nd_frame *nd_fqueue_pop(nd_fqueue *q, int timeout_ms)
{
    nd_frame *f;
    uint64_t deadline = 0;
    int infinite = timeout_ms < 0;

    if (!infinite)
        deadline = nd_monotonic_ms() + (uint64_t)timeout_ms;

    nd_mutex_lock(&q->lock);
    while (!q->head && !q->closed) {
        if (infinite) {
            nd_cond_wait(&q->cond, &q->lock);
        } else {
            uint64_t now = nd_monotonic_ms();
            if (now >= deadline) {
                nd_mutex_unlock(&q->lock);
                return NULL;
            }
            nd_cond_timedwait(&q->cond, &q->lock, (int)(deadline - now));
        }
    }
    f = q->head;
    if (f) {
        q->head = f->next;
        if (!q->head)
            q->tail = NULL;
        q->bytes -= f->len;
        nd_cond_broadcast(&q->cond);
    }
    nd_mutex_unlock(&q->lock);
    return f;
}

void nd_fqueue_close(nd_fqueue *q)
{
    nd_mutex_lock(&q->lock);
    q->closed = 1;
    nd_cond_broadcast(&q->cond);
    nd_mutex_unlock(&q->lock);
}

size_t nd_fqueue_bytes(nd_fqueue *q)
{
    size_t n;
    nd_mutex_lock(&q->lock);
    n = q->bytes;
    nd_mutex_unlock(&q->lock);
    return n;
}
