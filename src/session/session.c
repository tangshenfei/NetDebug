#include "session/session.h"
#include "session/conn.h"
#include "core/error.h"
#include "core/log.h"
#include "core/util.h"
#include "net/iface.h"
#include "net/resolver.h"
#include "platform/netio.h"
#include "platform/platform.h"
#include "platform/serial.h"
#include "proto/ssh_client.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nd_mutex g_lock;
static int g_lock_ready = 0;
static nd_session *g_sessions = NULL;
static uint64_t g_next_id = 1;

static void registry_init(void)
{
    if (!g_lock_ready) {
        nd_mutex_init(&g_lock);
        g_lock_ready = 1;
    }
}

nd_session *nd_session_new(const char *name, const char *host, uint16_t port,
                           nd_family family, const char *iface_id,
                           const char *src_addr, int transport, int telnet)
{
    nd_session *s;
    registry_init();

    s = (nd_session *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    nd_strlcpy(s->name, name && name[0] ? name : host, sizeof(s->name));
    nd_strlcpy(s->host, host, sizeof(s->host));
    s->port = port;
    s->family = family;
    s->transport = transport;
    s->telnet = telnet;
    s->baud = 115200;
    s->data_bits = 8;
    s->stop_bits = 1;
    nd_strlcpy(s->iface_id, iface_id ? iface_id : "", sizeof(s->iface_id));
    nd_strlcpy(s->src_addr, src_addr ? src_addr : "", sizeof(s->src_addr));
    s->created_ms = nd_monotonic_ms();
    atomic_init(&s->refs, 1);
    atomic_init(&s->state, ND_SESS_IDLE);
    atomic_init(&s->busy, 0);
    nd_mutex_init(&s->lock);

    nd_mutex_lock(&g_lock);
    s->id = g_next_id++;
    s->next = g_sessions;
    g_sessions = s;
    nd_mutex_unlock(&g_lock);
    return s;
}

static int pick_source(nd_session *s, const nd_addr *target, nd_addr *src,
                       char *err, size_t errlen)
{
    memset(src, 0, sizeof(*src));

    if (s->src_addr[0]) {
        if (nd_addr_parse(s->src_addr, 0, src) != 0) {
            snprintf(err, errlen, "源地址非法: %s", s->src_addr);
            return ND_E_SRC_BIND;
        }
        if (src->family != target->family) {
            snprintf(err, errlen, "源地址与目标地址族不一致");
            return ND_E_SRC_BIND;
        }
        src->port = 0;
        return 0;
    }

    if (s->iface_id[0]) {
        nd_iface ifs[ND_MAX_IFACE];
        size_t n = 0;
        int idx;
        if (nd_iface_list(ifs, ND_MAX_IFACE, &n) != 0) {
            snprintf(err, errlen, "枚举网卡失败");
            return ND_E_SRC_BIND;
        }
        idx = nd_iface_find(ifs, n, s->iface_id);
        if (idx < 0) {
            snprintf(err, errlen, "网卡不存在: %s", s->iface_id);
            return ND_E_PARAM_IFACE;
        }
        for (size_t i = 0; i < ifs[idx].n_addr; i++) {
            if (ifs[idx].addrs[i].family == target->family) {
                *src = ifs[idx].addrs[i];
                src->port = 0;
                return 0;
            }
        }
        snprintf(err, errlen, "该网卡没有%s地址",
                 target->family == AF_INET ? " IPv4 " : " IPv6 ");
        return ND_E_SRC_NOADDR;
    }

    return 0; /* 不绑定源地址 */
}

int nd_session_connect(nd_session *s, uint64_t *conn_id, char *err, size_t errlen)
{
    nd_addr addrs[16];
    size_t n = 0;
    char rerr[256] = "";
    int last_err = ND_E_CONNECT_ALL;
    int last_sys = 0;
    int fd = -1;
    nd_addr used;
    nd_addr local;
    nd_conn *c = NULL;

    if (!s)
        return ND_E_PARAM;
    *conn_id = 0;

    if (atomic_exchange(&s->busy, 1)) {
        snprintf(err, errlen, "会话正在连接");
        return ND_E_PARAM;
    }

    nd_mutex_lock(&s->lock);
    s->last_error[0] = '\0';
    nd_mutex_unlock(&s->lock);
    atomic_store(&s->state, ND_SESS_CONNECTING);

    if (s->ssh) {
        int fd = -1;
        char serr[512] = "";
        nd_ssh *sh;
        nd_conn *sc;
        nd_addr local, peer;

        sh = nd_ssh_connect(s->host, s->port, s->family, s->ssh_user, s->ssh_pass,
                            s->ssh_key, s->ssh_compat, 8000, &fd, serr, sizeof(serr));
        if (!sh) {
            snprintf(err, errlen, "%s", serr);
            last_err = ND_E_PROTO_AUTH;
            goto fail;
        }
        if (nd_ssh_shell(sh, s->ssh_cols > 0 ? s->ssh_cols : 120,
                         s->ssh_rows > 0 ? s->ssh_rows : 32,
                         serr, sizeof(serr)) != 0) {
            nd_ssh_close(sh);
            snprintf(err, errlen, "%s", serr);
            last_err = ND_E_PROTO;
            goto fail;
        }
        memset(&local, 0, sizeof(local));
        nd_net_getsockname(fd, &local);
        memset(&peer, 0, sizeof(peer));
        nd_net_getpeername(fd, &peer);
        sc = nd_conn_new(fd, peer.family ? &peer : NULL, &local, 0, "ssh", 0);
        if (!sc) {
            nd_ssh_close(sh);
            snprintf(err, errlen, "创建 SSH 连接失败");
            last_err = ND_E_INTERNAL;
            goto fail;
        }
        sc->ssh = sh;
        sc->owns_fd = 0; /* nd_ssh_close 负责关闭 socket */
        nd_strlcpy(s->ssh_fp, nd_ssh_fingerprint(sh), sizeof(s->ssh_fp));
        if (nd_conn_start(sc) != 0) {
            nd_conn_close(sc);
            snprintf(err, errlen, "启动 SSH 接收线程失败");
            last_err = ND_E_IO_THREAD;
            goto fail;
        }
        nd_mutex_lock(&s->lock);
        s->conn_id = sc->id;
        nd_mutex_unlock(&s->lock);
        atomic_store(&s->state, ND_SESS_OPEN);
        atomic_store(&s->busy, 0);
        *conn_id = sc->id;
        ND_LOGI("session %llu ssh to %s:%u as %s", (unsigned long long)s->id,
                s->host, (unsigned)s->port, s->ssh_user);
        return ND_OK;
    }

    if (s->serial) {
        nd_serial *sp;
        char serr[256] = "";
        nd_conn *sc;

        sp = nd_serial_open(s->serial_port, s->baud, s->data_bits, s->parity,
                            s->stop_bits, s->flow, s->dtr, s->rts,
                            serr, sizeof(serr));
        if (!sp) {
            snprintf(err, errlen, "%s", serr);
            last_err = ND_E_SERIAL_OPEN;
            goto fail;
        }
        sc = nd_conn_new(-1, NULL, NULL, 0, "serial", 0);
        if (!sc) {
            nd_serial_close(sp);
            snprintf(err, errlen, "创建串口连接失败");
            last_err = ND_E_SERIAL_IO;
            goto fail;
        }
        sc->serial = sp;
        if (nd_conn_start(sc) != 0) {
            nd_conn_close(sc);
            snprintf(err, errlen, "启动串口接收线程失败");
            last_err = ND_E_SERIAL_IO;
            goto fail;
        }
        nd_mutex_lock(&s->lock);
        s->conn_id = sc->id;
        nd_mutex_unlock(&s->lock);
        atomic_store(&s->state, ND_SESS_OPEN);
        atomic_store(&s->busy, 0);
        *conn_id = sc->id;
        ND_LOGI("session %llu opened serial %s @%d",
                (unsigned long long)s->id, s->serial_port, s->baud);
        return ND_OK;
    }

    if (nd_resolve(s->host, s->port, s->family, addrs, 16, &n, rerr, sizeof(rerr)) != 0) {
        last_err = ND_E_RESOLVE;
        snprintf(err, errlen, "%s", rerr);
        goto fail;
    }

    memset(&used, 0, sizeof(used));
    for (size_t i = 0; i < n; i++) {
        nd_addr src;
        int se = 0;
        int socktype = s->transport ? SOCK_DGRAM : SOCK_STREAM;
        int rc = pick_source(s, &addrs[i], &src, err, errlen);
        if (rc != ND_OK && (s->src_addr[0] || s->iface_id[0])) {
            last_err = rc;
            last_sys = 0;
            continue;
        }

        fd = nd_net_socket(addrs[i].family, socktype);
        if (fd < 0) {
            last_err = ND_E_CONNECT_ALL;
            last_sys = nd_net_last_error();
            continue;
        }
        if (src.family == AF_INET || src.family == AF_INET6) {
            if (nd_net_bind(fd, &src) != 0) {
                last_err = ND_E_SRC_BIND;
                last_sys = nd_net_last_error();
                nd_net_close(fd);
                fd = -1;
                continue;
            }
        } else if (s->transport) {
            /* UDP 没有源地址时也显式绑定通配地址，保证本地端口确定 */
            nd_addr any;
            memset(&any, 0, sizeof(any));
            any.family = addrs[i].family;
            any.port = 0;
            if (nd_net_bind(fd, &any) != 0) {
                last_err = ND_E_SRC_BIND;
                last_sys = nd_net_last_error();
                nd_net_close(fd);
                fd = -1;
                continue;
            }
        }
        if (nd_net_connect(fd, &addrs[i], s->transport ? 1000 : 3000, &se) != 0) {
            last_err = ND_E_CONNECT_TIMEOUT;
            last_sys = se;
            nd_net_close(fd);
            fd = -1;
            continue;
        }
        used = addrs[i];
        break;
    }

    if (fd < 0) {
        snprintf(err, errlen, "%s（共尝试 %zu 个地址）", nd_error_str(last_err), n);
        goto fail;
    }

    nd_net_getsockname(fd, &local);
    c = nd_conn_new(fd, &used, &local, 0,
                    s->transport ? "udp-client" :
                    (s->telnet ? "telnet" : "tcp-client"), 0);
    if (!c) {
        nd_net_close(fd);
        last_err = ND_E_INTERNAL;
        snprintf(err, errlen, "创建连接失败");
        goto fail;
    }
    if (s->telnet)
        nd_conn_enable_telnet(c, 0);
    if (nd_conn_start(c) != 0) {
        nd_net_close(fd);
        nd_conn_put(c);
        last_err = ND_E_IO_THREAD;
        snprintf(err, errlen, "启动接收线程失败");
        goto fail;
    }

    nd_mutex_lock(&s->lock);
    s->conn_id = c->id;
    s->last_error[0] = '\0';
    nd_mutex_unlock(&s->lock);
    atomic_store(&s->state, ND_SESS_OPEN);
    atomic_store(&s->busy, 0);
    *conn_id = c->id;
    ND_LOGI("session %llu connected to %s:%u (conn %llu)",
            (unsigned long long)s->id, s->host, (unsigned)s->port,
            (unsigned long long)c->id);
    return ND_OK;

fail:
    nd_mutex_lock(&s->lock);
    snprintf(s->last_error, sizeof(s->last_error), "%s", err);
    nd_mutex_unlock(&s->lock);
    atomic_store(&s->state, ND_SESS_ERROR);
    atomic_store(&s->busy, 0);
    (void)last_sys;
    return last_err;
}

void nd_session_disconnect(nd_session *s)
{
    uint64_t cid;
    if (!s)
        return;
    nd_mutex_lock(&s->lock);
    cid = s->conn_id;
    s->conn_id = 0;
    nd_mutex_unlock(&s->lock);
    if (cid) {
        nd_conn *c = nd_conn_get(cid);
        if (c) {
            nd_conn_close(c);
            nd_conn_put(c);
        }
    }
    atomic_store(&s->state, ND_SESS_IDLE);
}

void nd_session_sync(nd_session *s)
{
    uint64_t cid;
    nd_conn *c;
    if (!s)
        return;
    nd_mutex_lock(&s->lock);
    cid = s->conn_id;
    nd_mutex_unlock(&s->lock);
    if (!cid)
        return;
    c = nd_conn_get(cid);
    if (c) {
        nd_conn_put(c);
        return;
    }
    nd_mutex_lock(&s->lock);
    s->conn_id = 0;
    nd_mutex_unlock(&s->lock);
    atomic_store(&s->state, ND_SESS_IDLE);
}

void nd_session_free(nd_session *s)
{
    if (!s)
        return;
    nd_session_disconnect(s);
    nd_mutex_lock(&g_lock);
    {
        nd_session **pp = &g_sessions;
        while (*pp && *pp != s)
            pp = &(*pp)->next;
        if (*pp)
            *pp = s->next;
    }
    nd_mutex_unlock(&g_lock);
    nd_session_put(s);
}

nd_session *nd_session_get(uint64_t id)
{
    nd_session *p;
    registry_init();
    nd_mutex_lock(&g_lock);
    for (p = g_sessions; p; p = p->next) {
        if (p->id == id) {
            atomic_fetch_add(&p->refs, 1);
            break;
        }
    }
    nd_mutex_unlock(&g_lock);
    return p;
}

void nd_session_put(nd_session *s)
{
    if (!s)
        return;
    if (atomic_fetch_sub(&s->refs, 1) == 1) {
        nd_mutex_destroy(&s->lock);
        free(s);
    }
}

size_t nd_session_list(uint64_t *ids, size_t cap)
{
    nd_session *p;
    size_t n = 0;
    registry_init();
    nd_mutex_lock(&g_lock);
    for (p = g_sessions; p && n < cap; p = p->next)
        ids[n++] = p->id;
    nd_mutex_unlock(&g_lock);
    return n;
}

int nd_session_get_state(nd_session *s) { return atomic_load(&s->state); }

uint64_t nd_session_get_conn(nd_session *s)
{
    uint64_t v;
    nd_mutex_lock(&s->lock);
    v = s->conn_id;
    nd_mutex_unlock(&s->lock);
    return v;
}
