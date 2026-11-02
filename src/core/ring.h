#ifndef ND_RING_H
#define ND_RING_H

#include <stddef.h>

/* 单生产者/单消费者环形缓冲 */
typedef struct {
    unsigned char *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t used;
} nd_ring;

int    nd_ring_init(nd_ring *r, size_t cap);
void   nd_ring_free(nd_ring *r);
size_t nd_ring_write(nd_ring *r, const void *data, size_t len);
size_t nd_ring_read(nd_ring *r, void *out, size_t len);
size_t nd_ring_peek(const nd_ring *r, void *out, size_t len);
size_t nd_ring_used(const nd_ring *r);
size_t nd_ring_space(const nd_ring *r);
void   nd_ring_clear(nd_ring *r);

#endif /* ND_RING_H */
