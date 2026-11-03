#include "session/service.h"
#include "core/error.h"
#include "core/log.h"
#include "core/util.h"
#include "platform/netio.h"
#include "platform/platform.h"
#include "proto/ftp_server.h"
#include "proto/tftp_server.h"
#include "proto/http_server.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static nd_mutex g_lock;
static int g_lock_ready = 0;
static nd_service *g_services = NULL;
static uint64_t g_next_service_id = 1;

static void svc_registry_init(void)
{
    if (!g_lock_ready) {
        nd_mutex_init(&g_lock);
        g_lock_ready = 1;
    }
}

static int svc_add_conn(nd_service *s, nd_conn *c)
{
    int ok = 0;
    nd_mutex_lock(&s->lock);
    if (s->n_conns < s->max_conns) {
        if (s->n_conns == s->cap_conns) {
            size_t ncap = s->cap_conns ? s->cap_conns * 2 : 16;
            nd_conn **np = (nd_conn **)realloc(s->conns, ncap * sizeof(*np));
            if (np) {
                s->conns = np;
                s->cap_conns = ncap;
            }
        }
        if (s->n_conns < s->cap_conns) {
            s->conns[s->n_conns++] = c;
            ok = 1;
            if (atomic_load(&s->peak_conns) < (int)s->n_conns)
                atomic_store(&s->peak_conns, (int)s->n_conns);
        }
    }
    nd_mutex_unlock(&s->lock);
    return ok;
}

void nd_service_detach_conn(uint64_t service_id, nd_conn *c)
{
    nd_service *s = nd_service_get(service_id);
    if (s) {
        nd_mutex_lock(&s->lock);
        for (size_t i = 0; i < s->n_conns; i++) {
            if (s->conns[i] == c) {
                s->conns[i] = s->conns[--s->n_conns];
                break;
            }
        }
        nd_mutex_unlock(&s->lock);
        nd_service_put(s);
    }
    nd_conn_put(c);
}

static nd_conn *svc_conn_by_fd(nd_service *s, int fd){
    nd_conn *found = NULL;
    nd_mutex_lock(&s->lock);
    for (size_t i = 0; i < s->n_conns; i++) {
        if (s->conns[i]->fd == fd) {
            found = s->conns[i];
            break;
        }
    }
    nd_mutex_unlock(&s->lock);
    return found;
}

static void *accept_thread(void *arg)
{
    nd_service *s = (nd_service *)arg;
    nd_pollfd fds[ND_MAX_LISTEN_SOCKS];
    size_t n = 0, i;
    int is_udp = (s->bind.type == SOCK_DGRAM);

    for (i = 0; i < s->plan.n; i++) {
        if (s->plan.slot[i].fd >= 0) {
            fds[n].fd = s->plan.slot[i].fd;
            fds[n].events = ND_POLLIN;
            fds[n].revents = 0;
            n++;
        }
    }
    ND_LOGI("service %llu %s on %zu socket(s)", (unsigned long long)s->id,
            is_udp ? "udp listening" : "listening", n);

    while (!atomic_load(&s->stopping)) {
        int rc = nd_net_poll(fds, n, 500);
        if (rc <= 0)
            continue;
        for (i = 0; i < n; i++) {
            if (!(fds[i].revents & ND_POLLIN))
                continue;
            if (is_udp) {
                unsigned char buf[65536];
                nd_addr peer;
                int rn;
                nd_conn *c;
                while (!atomic_load(&s->stopping)) {
                    memset(&peer, 0, sizeof(peer));
                    rn = nd_net_recvfrom(fds[i].fd, buf, sizeof(buf), &peer, 0);
                    if (rn <= 0)
                        break;
                    if (!strcmp(s->proto, "tftp")) {
                        nd_tftp_cfg cfg;
                        nd_addr laddr;
                        size_t k;
                        memset(&laddr, 0, sizeof(laddr));
                        for (k = 0; k < s->plan.n; k++) {
                            if (s->plan.slot[k].fd == fds[i].fd) {
                                laddr = s->plan.slot[k].local;
                                break;
                            }
                        }
                        memset(&cfg, 0, sizeof(cfg));
                        nd_strlcpy(cfg.root, s->tftp_root, sizeof(cfg.root));
                        cfg.readonly = s->tftp_readonly;
                        cfg.timeout_s = s->tftp_timeout;
                        nd_tftp_server_start(fds[i].fd, &laddr, &peer, buf, (size_t)rn, &cfg);
                        continue;
                    }
                    c = svc_conn_by_fd(s, fds[i].fd);
                    if (c)
                        nd_conn_note_rx(c, buf, (size_t)rn, &peer);
                    /* 非阻塞套接字：继续读直到 EWOULDBLOCK */
                }
                continue;
            }
            for (;;) {
                nd_addr peer, local;
                int cfd;
                nd_conn *c;

                if (atomic_load(&s->stopping))
                    break;
                cfd = nd_net_accept(fds[i].fd, &peer, &local);
                if (cfd < 0)
                    break;

                if (!strcmp(s->proto, "ftp")) {
                    nd_ftp_srv_cfg cfg;
                    memset(&cfg, 0, sizeof(cfg));
                    nd_strlcpy(cfg.root, s->ftp_root, sizeof(cfg.root));
                    cfg.readonly = s->ftp_readonly;
                    cfg.anonymous = s->ftp_anonymous;
                    nd_strlcpy(cfg.user, s->ftp_user, sizeof(cfg.user));
                    nd_strlcpy(cfg.pass, s->ftp_pass, sizeof(cfg.pass));
                    cfg.pasv_min = s->pasv_min;
                    cfg.pasv_max = s->pasv_max;
                    nd_strlcpy(cfg.advertise, s->ftp_advertise, sizeof(cfg.advertise));
                    if (nd_ftp_server_start(cfd, &peer, &local, &cfg) != 0)
                        nd_net_close(cfd);
                    continue;
                }
                if (!strcmp(s->proto, "http") || !strcmp(s->proto, "speedtest")) {
                    nd_http_srv_cfg cfg;
                    memset(&cfg, 0, sizeof(cfg));
                    nd_strlcpy(cfg.root, s->http_root, sizeof(cfg.root));
                    cfg.echo = s->echo;
                    cfg.speedtest = !strcmp(s->proto, "speedtest");
                    atomic_fetch_add(&s->refs, 1);
                    if (nd_http_server_start(cfd, s, &cfg) != 0) {
                        nd_net_close(cfd);
                    }
                    continue;
                }

                c = nd_conn_new(cfd, &peer, &local, s->id,
                                s->proto[0] ? s->proto : "raw-tcp", s->echo);
                if (!c) {
                    nd_net_close(cfd);
                    continue;
                }
                if (!svc_add_conn(s, c)) {
                    nd_net_shutdown(cfd);
                    nd_net_close(cfd);
                    nd_conn_put(c);
                    continue;
                }
                if (!strcmp(s->proto, "telnet"))
                    nd_conn_enable_telnet(c, 1);
                if (nd_conn_start(c) != 0) {
                    ND_LOGE("service %llu: cannot start conn thread",
                            (unsigned long long)s->id);
                    nd_conn_close(c);
                }
            }
        }
    }

    ND_LOGI("service %llu accept loop exit", (unsigned long long)s->id);
    nd_service_put(s);
    return NULL;
}

nd_service *nd_service_new(const char *name, const char *proto,
                           const nd_bind_spec *bind, int echo)
{
    nd_service *s;
    svc_registry_init();

    s = (nd_service *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->bind = *bind;
    s->echo = echo;
    s->max_conns = 64;
    s->pasv_min = 50000;
    s->pasv_max = 50100;
    s->ftp_anonymous = 1;
    s->tftp_timeout = 3;
    nd_strlcpy(s->ftp_user, "ftp", sizeof(s->ftp_user));
    {
        char defroot[600];
        snprintf(defroot, sizeof(defroot), "%sftp-root", nd_config_dir());
        nd_strlcpy(s->ftp_root, defroot, sizeof(s->ftp_root));
        snprintf(defroot, sizeof(defroot), "%stftp-root", nd_config_dir());
        nd_strlcpy(s->tftp_root, defroot, sizeof(s->tftp_root));
        snprintf(defroot, sizeof(defroot), "%shttp-root", nd_config_dir());
        nd_strlcpy(s->http_root, defroot, sizeof(s->http_root));
    }
    nd_strlcpy(s->name, name && name[0] ? name : "service", sizeof(s->name));
    nd_strlcpy(s->proto, proto ? proto : "raw-tcp", sizeof(s->proto));
    atomic_init(&s->refs, 1);
    atomic_init(&s->running, 0);
    atomic_init(&s->stopping, 0);
    atomic_init(&s->http_active, 0);
    atomic_init(&s->http_requests, 0);
    atomic_init(&s->started_ms, 0);
    atomic_init(&s->peak_conns, 0);
    nd_mutex_init(&s->lock);

    nd_mutex_lock(&g_lock);
    s->id = g_next_service_id++;
    s->next = g_services;
    g_services = s;
    nd_mutex_unlock(&g_lock);
    return s;
}

int nd_service_start(nd_service *s, char *err, size_t errlen)
{
    nd_iface ifs[ND_MAX_IFACE];
    size_t n_ifs = 0;
    int rc;

    if (atomic_load(&s->running))
        return ND_OK;
    if (nd_iface_list(ifs, ND_MAX_IFACE, &n_ifs) != 0)
        return ND_E_INTERNAL;
    if (!strcmp(s->proto, "ftp"))
        nd_mkdir_p(s->ftp_root);
    if (!strcmp(s->proto, "tftp"))
        nd_mkdir_p(s->tftp_root);
    if (!strcmp(s->proto, "http"))
        nd_mkdir_p(s->http_root);

    memset(&s->plan, 0, sizeof(s->plan));
    rc = nd_plan_build(&s->bind, ifs, n_ifs, 1, &s->plan);
    if (rc != ND_OK) {
        if (err)
            snprintf(err, errlen, "%s", nd_error_str(rc));
        snprintf(s->last_error, sizeof(s->last_error), "%s", nd_error_str(rc));
        return rc;
    }

    if (s->bind.type == SOCK_DGRAM && strcmp(s->proto, "tftp") != 0) {        size_t made = 0;
        for (size_t i = 0; i < s->plan.n; i++) {
            nd_conn *c;
            if (s->plan.slot[i].fd < 0)
                continue;
            c = nd_conn_new(s->plan.slot[i].fd, NULL, &s->plan.slot[i].local,
                            s->id, s->proto[0] ? s->proto : "raw-udp", s->echo);
            if (!c)
                continue;
            nd_conn_set_datagram(c, 1, 0); /* 套接字由服务统一关闭 */
            if (!svc_add_conn(s, c)) {
                nd_conn_put(c);
                continue;
            }
            made++;
        }
        if (made == 0) {
            nd_plan_close(&s->plan);
            if (err)
                snprintf(err, errlen, "创建 UDP 通道失败");
            return ND_E_INTERNAL;
        }
    }

    atomic_store(&s->stopping, 0);
    atomic_store(&s->running, 1);
    atomic_store(&s->started_ms, nd_monotonic_ms());
    atomic_store(&s->peak_conns, 0);
    atomic_fetch_add(&s->refs, 1); /* accept 线程持有 */
    if (nd_thread_start(&s->th_accept, accept_thread, s, 256 * 1024) != 0) {
        atomic_fetch_sub(&s->refs, 1);
        nd_plan_close(&s->plan);
        atomic_store(&s->running, 0);
        return ND_E_IO_THREAD;
    }
    return ND_OK;
}

void nd_service_stop(nd_service *s)
{
    nd_conn **copy = NULL;
    size_t n = 0, i;

    if (!s)
        return;
    if (atomic_exchange(&s->stopping, 1))
        return;
    if (!atomic_load(&s->running))
        return;

    nd_thread_join(&s->th_accept);
    atomic_store(&s->running, 0);
    atomic_store(&s->started_ms, 0);

    nd_mutex_lock(&s->lock);
    n = s->n_conns;
    if (n) {
        copy = (nd_conn **)malloc(n * sizeof(*copy));
        if (copy) {
            memcpy(copy, s->conns, n * sizeof(*copy));
            for (size_t k = 0; k < n; k++)
                atomic_fetch_add(&copy[k]->refs, 1); /* 关闭期间自保 */
        }
        s->n_conns = 0;
    }
    nd_mutex_unlock(&s->lock);

    for (i = 0; i < n; i++) {
        if (copy[i]) {
            nd_conn_close(copy[i]); /* detach 时释放服务持有的引用 */
            nd_conn_put(copy[i]);   /* 释放本次停止流程的临时引用 */
        }
    }
    free(copy);
    nd_plan_close(&s->plan);
}

void nd_service_free(nd_service *s)
{
    if (!s)
        return;
    nd_service_stop(s);

    nd_mutex_lock(&g_lock);
    {
        nd_service **pp = &g_services;
        while (*pp && *pp != s)
            pp = &(*pp)->next;
        if (*pp)
            *pp = s->next;
    }
    nd_mutex_unlock(&g_lock);

    /* 注册表持有的引用；引用归零时析构 conns/lock/本体 */
    nd_service_put(s);
}

nd_service *nd_service_get(uint64_t id)
{
    nd_service *p;
    svc_registry_init();
    nd_mutex_lock(&g_lock);
    for (p = g_services; p; p = p->next) {
        if (p->id == id) {
            atomic_fetch_add(&p->refs, 1);
            break;
        }
    }
    nd_mutex_unlock(&g_lock);
    return p;
}

void nd_service_put(nd_service *s)
{
    if (!s)
        return;
    if (atomic_fetch_sub(&s->refs, 1) == 1) {
        free(s->conns);
        nd_mutex_destroy(&s->lock);
        free(s);
    }
}

size_t nd_service_list(uint64_t *ids, size_t cap)
{
    nd_service *p;
    size_t n = 0;
    svc_registry_init();
    nd_mutex_lock(&g_lock);
    for (p = g_services; p && n < cap; p = p->next)
        ids[n++] = p->id;
    nd_mutex_unlock(&g_lock);
    return n;
}

const char *nd_service_proto(const nd_service *s) { return s->proto; }

int nd_service_running(const nd_service *s) { return atomic_load(&s->running); }

size_t nd_service_conn_count(nd_service *s)
{
    size_t n;
    nd_mutex_lock(&s->lock);
    n = s->n_conns;
    nd_mutex_unlock(&s->lock);
    return n;
}
