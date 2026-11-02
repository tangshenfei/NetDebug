#include "net/addr.h"
#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <net/if.h>
#endif

int nd_addr_from_sockaddr(nd_addr *a, const struct sockaddr *sa, int salen)
{
    memset(a, 0, sizeof(*a));
    if (!sa)
        return -1;
    if (sa->sa_family == AF_INET && salen >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        a->family = AF_INET;
        a->port = ntohs(sin->sin_port);
        memcpy(a->raw, &sin->sin_addr, 4);
        return 0;
    }
    if (sa->sa_family == AF_INET6 && salen >= (int)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        a->family = AF_INET6;
        a->port = ntohs(sin6->sin6_port);
        a->scope_id = sin6->sin6_scope_id;
        memcpy(a->raw, &sin6->sin6_addr, 16);
        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
            a->is_mapped = 1;
        return 0;
    }
    return -1;
}

int nd_addr_to_sockaddr(const nd_addr *a, struct sockaddr_storage *ss, int *salen)
{
    memset(ss, 0, sizeof(*ss));
    if (a->family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)ss;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(a->port);
        memcpy(&sin->sin_addr, a->raw, 4);
        *salen = (int)sizeof(*sin);
        return 0;
    }
    if (a->family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(a->port);
        sin6->sin6_scope_id = a->scope_id;
        memcpy(&sin6->sin6_addr, a->raw, 16);
        *salen = (int)sizeof(*sin6);
        return 0;
    }
    return -1;
}

static int parse_port(const char *s, int *out)
{
    long v = 0;
    if (!s || !*s)
        return -1;
    while (*s) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535)
            return -1;
        s++;
    }
    *out = (int)v;
    return 0;
}

int nd_addr_parse(const char *text, int def_port, nd_addr *out)
{
    char tmp[256];
    char host[200];
    int port = def_port;
    int i, colons = 0;
    char *scope;
    unsigned long scope_val = 0;

    if (!text || !out)
        return -1;
    nd_strlcpy(tmp, text, sizeof(tmp));
    nd_trim(tmp);
    if (!tmp[0])
        return -1;

    memset(out, 0, sizeof(*out));

    if (tmp[0] == '[') {
        char *end = strchr(tmp, ']');
        if (!end)
            return -1;
        *end = '\0';
        nd_strlcpy(host, tmp + 1, sizeof(host));
        if (end[1] == ':') {
            if (parse_port(end + 2, &port) != 0)
                return -1;
        } else if (end[1] != '\0') {
            return -1;
        }
    } else {
        for (i = 0; tmp[i]; i++)
            if (tmp[i] == ':')
                colons++;
        if (colons == 1) {
            char *colon = strchr(tmp, ':');
            *colon = '\0';
            nd_strlcpy(host, tmp, sizeof(host));
            if (parse_port(colon + 1, &port) != 0)
                return -1;
        } else {
            nd_strlcpy(host, tmp, sizeof(host));
        }
    }

    scope = strchr(host, '%');
    if (scope) {
        *scope++ = '\0';
        if (!*scope)
            return -1;
        if (scope[0] >= '0' && scope[0] <= '9') {
            char *endp = NULL;
            scope_val = strtoul(scope, &endp, 10);
            if (!endp || *endp)
                return -1;
        } else {
#ifdef _WIN32
            return -1;
#else
            scope_val = if_nametoindex(scope);
            if (scope_val == 0)
                return -1;
#endif
        }
    }

    if (inet_pton(AF_INET, host, out->raw) == 1) {
        out->family = AF_INET;
        out->port = (uint16_t)port;
        return 0;
    }
    if (inet_pton(AF_INET6, host, out->raw) == 1) {
        out->family = AF_INET6;
        out->port = (uint16_t)port;
        out->scope_id = (uint32_t)scope_val;
        return 0;
    }
    return -1;
}

int nd_addr_format(const nd_addr *a, int with_port, char *buf, size_t cap)
{
    char ip[80];
    const void *src;
    int fam;

    if (buf && cap)
        buf[0] = '\0';
    if (!a)
        return -1;

    if (a->family == AF_INET) {
        fam = AF_INET;
        src = a->raw;
    } else if (a->family == AF_INET6 && a->is_mapped) {
        fam = AF_INET;
        src = a->raw + 12;
    } else if (a->family == AF_INET6) {
        fam = AF_INET6;
        src = a->raw;
    } else {
        return -1;
    }

    if (!inet_ntop(fam, src, ip, sizeof(ip)))
        return -1;

    if (fam == AF_INET) {
        if (with_port)
            snprintf(buf, cap, "%s:%u", ip, (unsigned)a->port);
        else
            snprintf(buf, cap, "%s", ip);
    } else {
        char sc[32] = "";
        if (a->scope_id)
            snprintf(sc, sizeof(sc), "%%%u", (unsigned)a->scope_id);
        if (with_port)
            snprintf(buf, cap, "[%s%s]:%u", ip, sc, (unsigned)a->port);
        else
            snprintf(buf, cap, "%s%s", ip, sc);
    }
    return 0;
}

int nd_addr_equal(const nd_addr *a, const nd_addr *b)
{
    if (a->family != b->family || a->port != b->port || a->scope_id != b->scope_id)
        return 0;
    return memcmp(a->raw, b->raw, a->family == AF_INET ? 4 : 16) == 0;
}

void nd_addr_normalize(nd_addr *a)
{
    if (a->family == AF_INET6 && a->is_mapped) {
        uint8_t v4[4];
        memcpy(v4, a->raw + 12, 4);
        memset(a->raw, 0, sizeof(a->raw));
        memcpy(a->raw, v4, 4);
        a->family = AF_INET;
        a->is_mapped = 0;
        a->scope_id = 0;
    }
}

int nd_addr_is_any(const nd_addr *a)
{
    static const uint8_t zero[16] = {0};
    if (a->family == AF_INET)
        return memcmp(a->raw, zero, 4) == 0;
    return memcmp(a->raw, zero, 16) == 0;
}

int nd_addr_is_loopback(const nd_addr *a)
{
    if (a->family == AF_INET)
        return a->raw[0] == 127;
    if (a->family == AF_INET6) {
        if (a->is_mapped)
            return a->raw[12] == 127;
        return memcmp(a->raw, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16) == 0;
    }
    return 0;
}

int nd_addr_is_linklocal(const nd_addr *a)
{
    if (a->family == AF_INET)
        return a->raw[0] == 169 && a->raw[1] == 254;
    if (a->family == AF_INET6)
        return a->raw[0] == 0xfe && (a->raw[1] & 0xc0) == 0x80;
    return 0;
}
