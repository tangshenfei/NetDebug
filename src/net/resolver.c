#include "net/resolver.h"
#include "core/error.h"

#include <stdio.h>
#include <string.h>

int nd_resolve(const char *host, uint16_t port, nd_family family,
               nd_addr *out, size_t cap, size_t *n,
               char *err, size_t errlen)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char service[16];
    size_t count = 0;
    int rc;

    *n = 0;
    if (!host || !host[0]) {
        if (err)
            snprintf(err, errlen, "主机名为空");
        return ND_E_PARAM;
    }

    /* 目标本身就是字面地址时，地址族由它自己决定：
     * 填了 2001:db8::1 还要再按 IPv4 去解析是没有意义的。 */
    {
        nd_addr lit;
        if (nd_addr_parse(host, port, &lit) == 0) {
            if (cap == 0) {
                if (err)
                    snprintf(err, errlen, "地址缓冲区不足");
                return ND_E_PARAM;
            }
            lit.port = port;
            out[0] = lit;
            *n = 1;
            return 0;
        }
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_family = family == ND_FAM_V4 ? AF_INET :
                      family == ND_FAM_V6 ? AF_INET6 : AF_UNSPEC;
    snprintf(service, sizeof(service), "%u", (unsigned)port);

    rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0 || !res) {
        if (err)
            snprintf(err, errlen, "解析失败: %s", host);
        return ND_E_RESOLVE;
    }

    for (struct addrinfo *ai = res; ai && count < cap; ai = ai->ai_next) {
        nd_addr a;
        if (nd_addr_from_sockaddr(&a, ai->ai_addr, (int)ai->ai_addrlen) != 0)
            continue;
        a.port = port;
        if (a.family == AF_INET6 && a.is_mapped)
            nd_addr_normalize(&a);
        if (family == ND_FAM_V4 && a.family != AF_INET)
            continue;
        if (family == ND_FAM_V6 && a.family != AF_INET6)
            continue;

        int dup = 0;
        for (size_t i = 0; i < count; i++) {
            if (nd_addr_equal(&out[i], &a)) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            out[count++] = a;
    }
    freeaddrinfo(res);

    *n = count;
    if (count == 0) {
        if (err)
            snprintf(err, errlen, "没有匹配的地址（%s）",
                     family == ND_FAM_V4 ? "仅 IPv4" :
                     family == ND_FAM_V6 ? "仅 IPv6" : "任意");
        return ND_E_RESOLVE;
    }
    return 0;
}
