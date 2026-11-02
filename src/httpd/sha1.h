#ifndef ND_SHA1_H
#define ND_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t total;
    unsigned char buf[64];
    size_t buflen;
} nd_sha1_ctx;

void nd_sha1_init(nd_sha1_ctx *c);
void nd_sha1_update(nd_sha1_ctx *c, const void *data, size_t len);
void nd_sha1_final(nd_sha1_ctx *c, unsigned char out[20]);

#endif /* ND_SHA1_H */
