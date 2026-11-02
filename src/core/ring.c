#include "core/ring.h"

#include <stdlib.h>
#include <string.h>

int nd_ring_init(nd_ring *r, size_t cap)
{
    if (cap < 2)
        cap = 2;
    r->buf = (unsigned char *)malloc(cap);
    if (!r->buf)
        return -1;
    r->cap = cap;
    r->head = r->tail = r->used = 0;
    return 0;
}

void nd_ring_free(nd_ring *r)
{
    free(r->buf);
    r->buf = NULL;
    r->cap = r->head = r->tail = r->used = 0;
}

size_t nd_ring_write(nd_ring *r, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t n = 0;
    while (n < len) {
        size_t space = r->cap - r->used;
        size_t chunk;
        if (space == 0)
            break;
        chunk = len - n;
        if (chunk > space)
            chunk = space;
        if (chunk > r->cap - r->head)
            chunk = r->cap - r->head;
        memcpy(r->buf + r->head, p + n, chunk);
        r->head = (r->head + chunk) % r->cap;
        r->used += chunk;
        n += chunk;
    }
    return n;
}

size_t nd_ring_read(nd_ring *r, void *out, size_t len)
{
    unsigned char *p = (unsigned char *)out;
    size_t n = 0;
    while (n < len && r->used > 0) {
        size_t chunk = r->cap - r->tail;
        if (chunk > len - n)
            chunk = len - n;
        if (chunk > r->used)
            chunk = r->used;
        memcpy(p + n, r->buf + r->tail, chunk);
        r->tail = (r->tail + chunk) % r->cap;
        r->used -= chunk;
        n += chunk;
    }
    return n;
}

size_t nd_ring_peek(const nd_ring *r, void *out, size_t len)
{
    unsigned char *p = (unsigned char *)out;
    size_t n = 0;
    size_t tail = r->tail;
    while (n < len && n < r->used) {
        size_t chunk = r->cap - tail;
        if (chunk > len - n)
            chunk = len - n;
        if (chunk > r->used - n)
            chunk = r->used - n;
        memcpy(p + n, r->buf + tail, chunk);
        tail = (tail + chunk) % r->cap;
        n += chunk;
    }
    return n;
}

size_t nd_ring_used(const nd_ring *r) { return r->used; }
size_t nd_ring_space(const nd_ring *r) { return r->cap - r->used; }
void nd_ring_clear(nd_ring *r) { r->head = r->tail = r->used = 0; }
