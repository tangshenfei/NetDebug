#ifndef ND_URL_H
#define ND_URL_H

#include <stdint.h>
#include <stddef.h>

typedef struct nd_url {
    char     scheme[8];
    char     host[256];
    uint16_t port;
    char     path[2048];
    int      tls;
} nd_url;

int nd_url_parse(const char *text, nd_url *out, char *err, size_t errlen);
/* 以 base 为基准解析 Location（绝对、/path、相对路径） */
int nd_url_resolve(const nd_url *base, const char *location, nd_url *out,
                   char *err, size_t errlen);

#endif /* ND_URL_H */
