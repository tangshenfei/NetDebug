#include "httpd/sha1.h"

#include <string.h>

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(nd_sha1_ctx *c, const unsigned char *p)
{
    uint32_t w[80];
    uint32_t a, b, d, e, f, k, tmp;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++)
        w[i] = ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->h[0];
    b = c->h[1];
    uint32_t cc = c->h[2];
    d = c->h[3];
    e = c->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        tmp = ROTL(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = ROTL(b, 30);
        b = a;
        a = tmp;
    }

    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

void nd_sha1_init(nd_sha1_ctx *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->total = 0;
    c->buflen = 0;
}

void nd_sha1_update(nd_sha1_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->total += len;
    if (c->buflen) {
        size_t need = 64 - c->buflen;
        if (need > len)
            need = len;
        memcpy(c->buf + c->buflen, p, need);
        c->buflen += need;
        p += need;
        len -= need;
        if (c->buflen == 64) {
            sha1_block(c, c->buf);
            c->buflen = 0;
        }
    }
    while (len >= 64) {
        sha1_block(c, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void nd_sha1_final(nd_sha1_ctx *c, unsigned char out[20])
{
    uint64_t bits = c->total * 8;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenbuf[8];
    int i;

    nd_sha1_update(c, &pad, 1);
    while (c->buflen != 56)
        nd_sha1_update(c, &zero, 1);

    for (i = 0; i < 8; i++)
        lenbuf[i] = (unsigned char)(bits >> (56 - i * 8));
    nd_sha1_update(c, lenbuf, 8);

    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}
