#ifndef ND_ADDR_H
#define ND_ADDR_H

#include "net/net.h"

#include <stddef.h>
#include <stdint.h>

typedef struct nd_addr {
    int      family;     /* AF_INET / AF_INET6 */
    int      is_mapped;  /* ::ffff:a.b.c.d */
    uint16_t port;       /* 主机序 */
    uint32_t scope_id;   /* IPv6 链路本地 */
    uint8_t  prefix;     /* 前缀长度（展示用） */
    uint8_t  raw[16];    /* 网络序 */
} nd_addr;

int  nd_addr_from_sockaddr(nd_addr *a, const struct sockaddr *sa, int salen);
int  nd_addr_to_sockaddr(const nd_addr *a, struct sockaddr_storage *ss, int *salen);

/* 支持 "[2001:db8::1]:80"、"2001:db8::1"、"fe80::1%12"、"1.2.3.4:80" */
int  nd_addr_parse(const char *text, int def_port, nd_addr *out);
int  nd_addr_format(const nd_addr *a, int with_port, char *buf, size_t cap);

int  nd_addr_equal(const nd_addr *a, const nd_addr *b);
void nd_addr_normalize(nd_addr *a);
int  nd_addr_is_any(const nd_addr *a);
int  nd_addr_is_loopback(const nd_addr *a);
int  nd_addr_is_linklocal(const nd_addr *a);

#endif /* ND_ADDR_H */
