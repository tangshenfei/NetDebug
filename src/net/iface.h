#ifndef ND_IFACE_H
#define ND_IFACE_H

#include "net/addr.h"

#include <stddef.h>

#define ND_MAX_IFACE       64
#define ND_MAX_IFACE_ADDRS 32

typedef struct nd_iface {
    char     id[80];      /* "win:{GUID}" / "linux:eth0" */
    char     name[128];   /* 展示名 */
    char     mac[20];
    uint32_t index;
    int      up;
    uint32_t mtu;
    size_t   n_addr;
    nd_addr  addrs[ND_MAX_IFACE_ADDRS];
} nd_iface;

int nd_iface_list(nd_iface *out, size_t cap, size_t *n);
int nd_iface_find(const nd_iface *ifs, size_t n, const char *id);
int nd_iface_index_by_name(const char *name);

#endif /* ND_IFACE_H */
