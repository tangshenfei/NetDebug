#include "httpd/base64.h"

#include <stdint.h>

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int nd_base64_encode(const void *data, size_t len, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i, o = 0;
    if (cap < ((len + 2) / 3) * 4 + 1)
        return -1;
    for (i = 0; i + 2 < len; i += 3) {
        out[o++] = b64tab[p[i] >> 2];
        out[o++] = b64tab[((p[i] & 0x03) << 4) | (p[i + 1] >> 4)];
        out[o++] = b64tab[((p[i + 1] & 0x0f) << 2) | (p[i + 2] >> 6)];
        out[o++] = b64tab[p[i + 2] & 0x3f];
    }
    if (i < len) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)p[i + 1] << 8;
        out[o++] = b64tab[(v >> 18) & 0x3f];
        out[o++] = b64tab[(v >> 12) & 0x3f];
        out[o++] = (i + 1 < len) ? b64tab[(v >> 6) & 0x3f] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return (int)o;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int nd_base64_decode(const char *in, void *out, size_t cap)
{
    unsigned char *p = (unsigned char *)out;
    size_t n = 0;
    int quad[4];
    int qn = 0;

    while (*in) {
        int v;
        if (*in == '=' || *in == '\r' || *in == '\n' || *in == ' ')
            break;
        v = b64val((unsigned char)*in++);
        if (v < 0)
            return -1;
        quad[qn++] = v;
        if (qn == 4) {
            uint32_t w = ((uint32_t)quad[0] << 18) | ((uint32_t)quad[1] << 12) |
                         ((uint32_t)quad[2] << 6) | (uint32_t)quad[3];
            if (n + 3 > cap)
                return -1;
            p[n++] = (unsigned char)(w >> 16);
            p[n++] = (unsigned char)(w >> 8);
            p[n++] = (unsigned char)w;
            qn = 0;
        }
    }
    if (qn == 2) {
        uint32_t w = ((uint32_t)quad[0] << 18) | ((uint32_t)quad[1] << 12);
        if (n + 1 > cap)
            return -1;
        p[n++] = (unsigned char)(w >> 16);
    } else if (qn == 3) {
        uint32_t w = ((uint32_t)quad[0] << 18) | ((uint32_t)quad[1] << 12) |
                     ((uint32_t)quad[2] << 6);
        if (n + 2 > cap)
            return -1;
        p[n++] = (unsigned char)(w >> 16);
        p[n++] = (unsigned char)(w >> 8);
    } else if (qn == 1) {
        return -1;
    }
    return (int)n;
}
