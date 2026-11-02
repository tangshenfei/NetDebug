#include "core/util.h"
#include "platform/platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

size_t nd_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = 0;
    if (!cap)
        return strlen(src);
    while (src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    return strlen(src);
}

char *nd_strdup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

void nd_trim(char *s)
{
    char *p = s;
    char *end;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
}

int nd_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int nd_ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

size_t nd_hex_encode(const void *data, size_t len, char *out, size_t cap, int upper)
{
    static const char low[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *tab = upper ? up : low;
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    if (cap < len * 2 + 1)
        return 0;
    for (i = 0; i < len; i++) {
        out[i * 2] = tab[p[i] >> 4];
        out[i * 2 + 1] = tab[p[i] & 0x0f];
    }
    out[len * 2] = '\0';
    return len * 2;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int nd_hex_decode(const char *hex, void *out, size_t cap)
{
    unsigned char *p = (unsigned char *)out;
    size_t n = 0;
    while (*hex) {
        int hi, lo;
        if (isspace((unsigned char)*hex)) {
            hex++;
            continue;
        }
        hi = hexval((unsigned char)hex[0]);
        lo = hexval((unsigned char)hex[1]);
        if (hi < 0 || lo < 0 || n >= cap)
            return -1;
        p[n++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return (int)n;
}

int nd_random_bytes(void *buf, size_t n)
{
    return nd_platform_random(buf, n);
}

int nd_utf8_valid(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xc2 && c <= 0xdf) {
            if (i + 1 >= len || (p[i + 1] & 0xc0) != 0x80)
                return 0;
            i += 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            if (i + 2 >= len)
                return 0;
            if ((p[i + 1] & 0xc0) != 0x80 || (p[i + 2] & 0xc0) != 0x80)
                return 0;
            if (c == 0xe0 && p[i + 1] < 0xa0)
                return 0;
            if (c == 0xed && p[i + 1] > 0x9f)
                return 0;
            i += 3;
        } else if (c >= 0xf0 && c <= 0xf4) {
            if (i + 3 >= len)
                return 0;
            if ((p[i + 1] & 0xc0) != 0x80 || (p[i + 2] & 0xc0) != 0x80 ||
                (p[i + 3] & 0xc0) != 0x80)
                return 0;
            if (c == 0xf0 && p[i + 1] < 0x90)
                return 0;
            if (c == 0xf4 && p[i + 1] > 0x8f)
                return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}
