#include "net/planner.h"
#include "platform/netio.h"
#include "core/error.h"
#include "core/log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

const char *nd_family_name(nd_family f)
{
    switch (f) {
    case ND_FAM_V4:   return "v4";
    case ND_FAM_V6:   return "v6";
    case ND_FAM_DUAL: return "dual";
    default:          return "?";
    }
}

static int classify_err(int sys)
{
#ifdef _WIN32
    if (sys == WSAEADDRINUSE)       return ND_E_BIND_INUSE;
    if (sys == WSAEACCES)           return ND_E_BIND_PERM;
    if (sys == WSAEADDRNOTAVAIL)    return ND_E_BIND_NOADDR;
#else
    if (sys == EADDRINUSE)          return ND_E_BIND_INUSE;
    if (sys == EACCES)              return ND_E_BIND_PERM;
    if (sys == EADDRNOTAVAIL)       return ND_E_BIND_NOADDR;
#endif
    return ND_E_BIND;
}

static int add_cand(nd_plan *p, const nd_addr *a)
{
    if (p->n >= ND_MAX_LISTEN_SOCKS)
        return -1;
    p->slot[p->n].fd = -1;
    p->slot[p->n].local = *a;
    p->slot[p->n].err = 0;
    p->slot[p->n].sys_errno = 0;
    p->n++;
    return 0;
}

static int expand_iface(const nd_iface *it, const nd_bind_spec *spec, nd_plan *p)
{
    size_t i;
    int have4 = 0, have6 = 0;

    for (i = 0; i < it->n_addr; i++) {
        int fam = it->addrs[i].family;
        if (fam == AF_INET)
            have4 = 1;
        else if (fam == AF_INET6)
            have6 = 1;

        if (spec->family == ND_FAM_V4 && fam != AF_INET)
            continue;
        if (spec->family == ND_FAM_V6 && fam != AF_INET6)
            continue;
        add_cand(p, &it->addrs[i]);
    }

    if (spec->family == ND_FAM_V4 && !have4) {
        p->fatal = 1;
        p->err = ND_E_BIND_NOADDR;
        return -1;
    }
    if (spec->family == ND_FAM_V6 && !have6) {
        p->fatal = 1;
        p->err = ND_E_BIND_NOADDR;
        return -1;
    }
    if (spec->family == ND_FAM_DUAL && (!have4 || !have6)) {
        if (!have4 && !have6) {
            p->fatal = 1;
            p->err = ND_E_BIND_NOADDR;
            return -1;
        }
        p->partial_family = 1;
        p->degraded = 1;
    }
    return 0;
}

int nd_plan_build(const nd_bind_spec *spec, const nd_iface *ifs, size_t n_ifs,
                  int do_bind, nd_plan *out)
{
    size_t i;
    int ok = 0;
    uint16_t use_port;
    int socktype;

    memset(out, 0, sizeof(*out));
    if (!spec) {
        out->fatal = 1;
        out->err = ND_E_PARAM;
        return ND_E_PARAM;
    }
    out->spec = *spec;
    out->err = 0;

    if (spec->family != ND_FAM_V4 && spec->family != ND_FAM_V6 && spec->family != ND_FAM_DUAL) {
        out->fatal = 1;
        out->err = ND_E_PARAM_FAMILY;
        return ND_E_PARAM_FAMILY;
    }
    if (spec->kind == ND_BIND_ADDR && spec->family == ND_FAM_DUAL) {
        out->fatal = 1;
        out->err = ND_E_BIND_FAMILY;
        return ND_E_BIND_FAMILY;
    }
    if (spec->kind == ND_BIND_ADDR) {
        if ((spec->family == ND_FAM_V4 && spec->addr.family != AF_INET) ||
            (spec->family == ND_FAM_V6 && spec->addr.family != AF_INET6)) {
            out->fatal = 1;
            out->err = ND_E_PARAM_FAMILY;
            return ND_E_PARAM_FAMILY;
        }
    }

    if (spec->kind == ND_BIND_ANY) {
        nd_addr a;
        memset(&a, 0, sizeof(a));
        a.port = spec->port;
        if (spec->family == ND_FAM_V4) {
            a.family = AF_INET;
            add_cand(out, &a);
        } else {
            a.family = AF_INET6;
            add_cand(out, &a);
        }
    } else if (spec->kind == ND_BIND_IFACE) {
        int idx = nd_iface_find(ifs, n_ifs, spec->iface_id);
        if (idx < 0) {
            out->fatal = 1;
            out->err = ND_E_PARAM_IFACE;
            return ND_E_PARAM_IFACE;
        }
        if (expand_iface(&ifs[idx], spec, out) != 0) {
            out->fatal = 1;
            return out->err;
        }
    } else {
        nd_addr a = spec->addr;
        a.port = spec->port;
        add_cand(out, &a);
    }

    if (out->n == 0) {
        out->fatal = 1;
        out->err = ND_E_BIND_NOADDR;
        return out->err;
    }

    if (!do_bind) {
        for (i = 0; i < out->n; i++) {
            out->slot[i].local.port = spec->port;
            out->slot[i].fd = -1;
        }
        return ND_OK;
    }

    socktype = spec->type ? spec->type : SOCK_STREAM;
    use_port = spec->port;

    for (i = 0; i < out->n; i++) {
        nd_plan_slot *s = &out->slot[i];
        int fd;
        int sys = 0;

        s->local.port = use_port;
        fd = nd_net_socket(s->local.family, socktype);
        if (fd < 0) {
            s->err = ND_E_BIND;
            s->sys_errno = nd_net_last_error();
            continue;
        }
        nd_net_set_reuse(fd);
        if (s->local.family == AF_INET6) {
            int dual = (spec->kind == ND_BIND_ANY && spec->family == ND_FAM_DUAL);
            nd_net_set_v6only(fd, dual ? 0 : 1);
        }

        if (nd_net_bind(fd, &s->local) != 0) {
            sys = nd_net_last_error();
            s->err = classify_err(sys);
            s->sys_errno = sys;
            nd_net_close(fd);
            continue;
        }
        if (socktype == SOCK_STREAM && nd_net_listen(fd, 128) != 0) {
            sys = nd_net_last_error();
            s->err = classify_err(sys);
            s->sys_errno = sys;
            nd_net_close(fd);
            continue;
        }
        nd_net_set_nonblock(fd, 1); /* accept / recvfrom 循环必须能返回 EWOULDBLOCK */
        if (nd_net_getsockname(fd, &s->local) != 0) {
            s->local.port = use_port;
            s->local.family = out->slot[i].local.family;
        }
        if (use_port == 0 && s->local.port != 0)
            use_port = s->local.port;

        s->fd = fd;
        s->err = 0;
        s->sys_errno = 0;
        ok++;
    }

    if (ok == 0) {
        out->fatal = 1;
        for (i = 0; i < out->n; i++) {
            if (out->slot[i].err) {
                out->err = out->slot[i].err;
                break;
            }
        }
        if (!out->err)
            out->err = ND_E_BIND_ALL;
        return out->err;
    }
    if (ok < (int)out->n)
        out->degraded = 1;

    return ND_OK;
}

void nd_plan_close(nd_plan *p)
{
    size_t i;
    for (i = 0; i < p->n; i++) {
        if (p->slot[i].fd >= 0) {
            nd_net_close(p->slot[i].fd);
            p->slot[i].fd = -1;
        }
    }
}

void nd_plan_format(const nd_plan *p, char *buf, size_t cap)
{
    size_t i, pos = 0;
    buf[0] = '\0';
    for (i = 0; i < p->n && pos + 4 < cap; i++) {
        char a[80];
        nd_addr_format(&p->slot[i].local, 1, a, sizeof(a));
        pos += (size_t)snprintf(buf + pos, cap - pos, "%s%s%s",
                                i ? ", " : "",
                                p->slot[i].fd >= 0 ? "" : "!",
                                a);
    }
}
