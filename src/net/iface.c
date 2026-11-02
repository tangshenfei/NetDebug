#include "net/iface.h"
#include "core/log.h"
#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <iphlpapi.h>

static void wide_to_utf8(const WCHAR *w, char *out, size_t cap)
{
    int n;
    if (!w || !*w) {
        if (cap)
            out[0] = '\0';
        return;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    if (n <= 0 && cap)
        out[0] = '\0';
}

static void mac_str(const unsigned char *p, size_t n, char *out, size_t cap)
{
    size_t i, pos = 0;
    out[0] = '\0';
    for (i = 0; i < n && pos + 4 < cap; i++) {
        pos += (size_t)snprintf(out + pos, cap - pos, i ? ":%02X" : "%02X", p[i]);
    }
}

int nd_iface_index_by_name(const char *name)
{
    (void)name;
    return 0;
}

int nd_iface_list(nd_iface *out, size_t cap, size_t *n)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *buf = NULL;
    IP_ADAPTER_ADDRESSES *ad;
    ULONG rc;
    size_t count = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        buf = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (!buf)
            return -1;
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST |
                                      GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, buf, &size);
        if (rc == NO_ERROR)
            break;
        free(buf);
        buf = NULL;
        if (rc != ERROR_BUFFER_OVERFLOW)
            return -1;
    }
    if (!buf)
        return -1;

    for (ad = buf; ad && count < cap; ad = ad->Next) {
        nd_iface *it = &out[count];
        IP_ADAPTER_UNICAST_ADDRESS *ua;

        memset(it, 0, sizeof(*it));
        snprintf(it->id, sizeof(it->id), "win:%s", ad->AdapterName);
        wide_to_utf8(ad->FriendlyName, it->name, sizeof(it->name));
        if (!it->name[0])
            nd_strlcpy(it->name, ad->AdapterName, sizeof(it->name));
        it->index = ad->IfIndex ? ad->IfIndex : ad->Ipv6IfIndex;
        it->up = (ad->OperStatus == IfOperStatusUp);
        it->mtu = ad->Mtu;
        mac_str(ad->PhysicalAddress, ad->PhysicalAddressLength, it->mac, sizeof(it->mac));

        for (ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
            nd_addr a;
            if (it->n_addr >= ND_MAX_IFACE_ADDRS)
                break;
            if (!ua->Address.lpSockaddr)
                continue;
            if (nd_addr_from_sockaddr(&a, ua->Address.lpSockaddr, (int)ua->Address.iSockaddrLength) != 0)
                continue;
            a.prefix = ua->OnLinkPrefixLength;
            it->addrs[it->n_addr++] = a;
        }
        count++;
    }
    free(buf);
    *n = count;
    return 0;
}

#else /* POSIX */

#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>

static int mask_prefix(const struct sockaddr *mask)
{
    const unsigned char *p;
    int bits = 0;
    int nbytes;
    if (!mask)
        return 0;
    if (mask->sa_family == AF_INET) {
        p = (const unsigned char *)&((const struct sockaddr_in *)mask)->sin_addr;
        nbytes = 4;
    } else if (mask->sa_family == AF_INET6) {
        p = (const unsigned char *)&((const struct sockaddr_in6 *)mask)->sin6_addr;
        nbytes = 16;
    } else {
        return 0;
    }
    for (int i = 0; i < nbytes; i++) {
        unsigned char b = p[i];
        while (b) {
            bits += b & 1;
            b >>= 1;
        }
    }
    return bits;
}

static int iface_mtu(const char *name)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    int mtu = 0;
    if (fd < 0)
        return 0;
    memset(&ifr, 0, sizeof(ifr));
    nd_strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
    if (ioctl(fd, SIOCGIFMTU, &ifr) == 0)
        mtu = ifr.ifr_mtu;
    close(fd);
    return mtu;
}

int nd_iface_index_by_name(const char *name)
{
    unsigned idx = if_nametoindex(name);
    return (int)idx;
}

int nd_iface_list(nd_iface *out, size_t cap, size_t *n)
{
    struct ifaddrs *ifa0 = NULL, *ifa;
    size_t count = 0;

    if (getifaddrs(&ifa0) != 0)
        return -1;

    for (ifa = ifa0; ifa && count < cap; ifa = ifa->ifa_next) {
        nd_iface *it = NULL;
        int i;

        if (!ifa->ifa_name)
            continue;
        for (i = 0; i < (int)count; i++) {
            if (strcmp(out[i].name, ifa->ifa_name) == 0) {
                it = &out[i];
                break;
            }
        }
        if (!it) {
            it = &out[count];
            memset(it, 0, sizeof(*it));
            snprintf(it->id, sizeof(it->id), "linux:%s", ifa->ifa_name);
            nd_strlcpy(it->name, ifa->ifa_name, sizeof(it->name));
            it->index = if_nametoindex(ifa->ifa_name);
            it->up = (ifa->ifa_flags & IFF_UP) ? 1 : 0;
            it->mtu = (uint32_t)iface_mtu(ifa->ifa_name);
            count++;
        }

        if (!ifa->ifa_addr)
            continue;

        if (ifa->ifa_addr->sa_family == AF_PACKET && !it->mac[0]) {
            const struct sockaddr_ll *sll = (const struct sockaddr_ll *)ifa->ifa_addr;
            size_t pos = 0;
            for (unsigned k = 0; k < sll->sll_halen && pos + 4 < sizeof(it->mac); k++)
                pos += (size_t)snprintf(it->mac + pos, sizeof(it->mac) - pos,
                                        k ? ":%02X" : "%02X", sll->sll_addr[k]);
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
            continue;

        if (it->n_addr < ND_MAX_IFACE_ADDRS) {
            nd_addr a;
            if (nd_addr_from_sockaddr(&a, ifa->ifa_addr,
                                      ifa->ifa_addr->sa_family == AF_INET
                                          ? (int)sizeof(struct sockaddr_in)
                                          : (int)sizeof(struct sockaddr_in6)) == 0) {
                a.prefix = (uint8_t)mask_prefix(ifa->ifa_netmask);
                if (a.family == AF_INET6 && nd_addr_is_linklocal(&a)) {
                    a.scope_id = (uint32_t)if_nametoindex(ifa->ifa_name);
                }
                it->addrs[it->n_addr++] = a;
            }
        }
    }

    freeifaddrs(ifa0);
    *n = count;
    return 0;
}

#endif

int nd_iface_find(const nd_iface *ifs, size_t n, const char *id)
{
    size_t i;
    if (!id)
        return -1;
    for (i = 0; i < n; i++) {
        if (strcmp(ifs[i].id, id) == 0)
            return (int)i;
    }
    return -1;
}
