#ifndef ND_RESOLVER_H
#define ND_RESOLVER_H

#include "net/addr.h"
#include "net/planner.h"

#include <stddef.h>
#include <stdint.h>

/* 解析主机名；family 决定只取 v4 / v6 / 两者。返回 0 表示至少有一个结果。 */
int nd_resolve(const char *host, uint16_t port, nd_family family,
               nd_addr *out, size_t cap, size_t *n,
               char *err, size_t errlen);

#endif /* ND_RESOLVER_H */
