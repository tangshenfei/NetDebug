#include "session/conn.h"
#include "session/service.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "httpd/ws.h"
#include "platform/netio.h"
#include "platform/platform.h"
#include "proto/telnet.h"
#include "proto/ssh_client.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static nd_mutex g_reg_lock;
static int g_reg_ready = 0;
static nd_conn *g_conns = NULL;
static uint64_t g_next_conn_id = 1;

static void conn_registry_init(void)
{
    if (!g_reg_ready) {
        nd_mutex_init(&g_reg_lock);
        g_reg_ready = 1;
    }
}

static void conn_log_open(nd_conn *c)
{
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%slogs", nd_config_dir());
    nd_mkdir_p(dir);
    snprintf(path, sizeof(path), "%s/conn-%llu.log", dir, (unsigned long long)c->id);
    c->log = fopen(path, "ab");
}

static void conn_log_write(nd_conn *c, int dir, const unsigned char *data, size_t len)
{
    time_t now;
    struct tm tm_now;
    char stamp[32];
    size_t i;

    if (!c->log)
        return;
    now = time(NULL);
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_now);

    nd_mutex_lock(&c->log_lock);
    fprintf(c->log, "[%s.%03u] %s %zu\n", stamp, (unsigned)(nd_monotonic_ms() % 1000),
            dir ? "TX" : "RX", len);
    for (i = 0; i < len; i += 16) {
        size_t j;
        fprintf(c->log, "  %04zx:", i);
        for (j = 0; j < 16 && i + j < len; j++)
            fprintf(c->log, " %02x", data[i + j]);
        fprintf(c->log, "\n");
    }
    fflush(c->log);
    nd_mutex_unlock(&c->log_lock);
}

static void conn_teardown(nd_conn *c)
{
    if (atomic_exchange(&c->closed, 1))
        return;

    if (c->log) {
        nd_mutex_lock(&c->log_lock);
        fflush(c->log);
        fclose(c->log);
        c->log = NULL;
        nd_mutex_unlock(&c->log_lock);
    }

    if (c->owns_fd && c->fd >= 0) {
        nd_net_shutdown(c->fd);
        nd_net_close(c->fd);
        c->fd = -1;
    }
    if (c->serial) {
        nd_serial_close(c->serial);
        c->serial = NULL;
    }
    if (c->ssh) {
        nd_ssh_close((nd_ssh *)c->ssh);
        c->ssh = NULL;
        c->fd = -1;
    }

    if (c->telnet) {
        nd_telnet_free((nd_telnet *)c->telnet);
        c->telnet = NULL;
    }

    /* 从全局表与所属服务表中摘除 */
    if (g_reg_ready) {
        nd_mutex_lock(&g_reg_lock);
        {
            nd_conn **pp = &g_conns;
            while (*pp && *pp != c)
                pp = &(*pp)->next;
            if (*pp)
                *pp = c->next;
        }
        nd_mutex_unlock(&g_reg_lock);
    }
    if (c->service_id)
        nd_service_detach_conn(c->service_id, c);
    else
        nd_conn_put(c); /* 无服务归属：释放 owner 引用 */

    nd_ws_state_event(c->id, 0);
}

void nd_conn_note_rx(nd_conn *c, const void *data, size_t n, const nd_addr *peer)
{
    if (!c || n == 0)
        return;
    if (peer) {
        nd_mutex_lock(&c->send_lock);
        c->last_peer = *peer;
        c->peer = *peer;
        nd_mutex_unlock(&c->send_lock);
    }
    c->rx_bytes += (uint64_t)n;
    c->last_rx_ms = nd_monotonic_ms();
    nd_ws_broadcast_data(c->id, 0, data, n);
    conn_log_write(c, 0, (const unsigned char *)data, n);
    if (c->echo)
        nd_conn_send(c, data, n);
}

static void *conn_rx_thread(void *arg)
{
    nd_conn *c = (nd_conn *)arg;
    unsigned char buf[65536];
    unsigned char clean[65536];

    while (!atomic_load(&c->closing)) {
        int n;
        if (c->ssh)
            n = nd_ssh_read((nd_ssh *)c->ssh, buf, sizeof(buf));
        else if (c->serial)
            n = nd_serial_read(c->serial, buf, sizeof(buf), 500);
        else
            n = nd_net_recv_some(c->fd, buf, sizeof(buf), 1000);
        if (n > 0) {
            if (c->telnet) {
                size_t outn = nd_telnet_feed((nd_telnet *)c->telnet, c, buf, (size_t)n,
                                             clean, sizeof(clean));
                if (outn)
                    nd_conn_note_rx(c, clean, outn, NULL);
            } else {
                nd_conn_note_rx(c, buf, (size_t)n, NULL);
            }
            continue;
        }
        if (n == -2)
            continue;
        break;
    }

    conn_teardown(c);
    nd_conn_put(c); /* 线程自持引用 */
    return NULL;
}

nd_conn *nd_conn_new(int fd, const nd_addr *peer, const nd_addr *local,
                     uint64_t service_id, const char *proto, int echo)
{
    nd_conn *c;
    conn_registry_init();

    c = (nd_conn *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->service_id = service_id;
    c->echo = echo;
    c->owns_fd = 1;
    nd_strlcpy(c->proto, proto ? proto : "raw", sizeof(c->proto));
    if (peer)
        c->peer = *peer;
    if (local)
        c->local = *local;
    c->started_ms = nd_monotonic_ms();
    if (fd >= 0)
        nd_net_set_timeouts(fd, 1000, 10000); /* UDP shutdown 不一定能唤醒 recv，必须有超时 */
    atomic_init(&c->refs, 1);
    atomic_init(&c->closing, 0);
    atomic_init(&c->closed, 0);
    nd_mutex_init(&c->send_lock);
    nd_mutex_init(&c->log_lock);

    nd_mutex_lock(&g_reg_lock);
    c->id = g_next_conn_id++;
    c->next = g_conns;
    g_conns = c;
    nd_mutex_unlock(&g_reg_lock);

    if (c->service_id) {
        char ev[256], peer_buf[96];
        nd_addr_format(&c->peer, 1, peer_buf, sizeof(peer_buf));
        snprintf(ev, sizeof(ev),
                 "{\"t\":\"conn_open\",\"svc\":%llu,\"conn\":%llu,\"proto\":\"%s\",\"peer\":\"%s\",\"ts\":%llu}",
                 (unsigned long long)c->service_id, (unsigned long long)c->id, c->proto, peer_buf,
                 (unsigned long long)nd_wall_ms());
        nd_ws_event_json(ev);
    }

    conn_log_open(c);
    return c;
}

int nd_conn_start(nd_conn *c)
{
    atomic_fetch_add(&c->refs, 1); /* rx 线程持有一份 */
    if (nd_thread_start(&c->th_rx, conn_rx_thread, c, 256 * 1024) != 0) {
        atomic_fetch_sub(&c->refs, 1);
        return -1;
    }
    c->started = 1;
    return 0;
}

int nd_conn_send(nd_conn *c, const void *data, size_t len)
{
    int rc;
    if (!c || atomic_load(&c->closing))
        return -1;
    if (!c->serial && c->fd < 0)
        return -1;
    nd_mutex_lock(&c->send_lock);
    if (c->ssh) {
        rc = nd_ssh_write((nd_ssh *)c->ssh, data, len);
    } else if (c->serial) {
        rc = nd_serial_write(c->serial, data, len);
    } else if (c->datagram) {
        if (c->last_peer.family == 0)
            rc = -1; /* 还没收到过数据报，不知道往哪发 */
        else
            rc = nd_net_sendto(c->fd, data, len, &c->last_peer, 10000);
    } else {
        rc = nd_net_send_all(c->fd, data, len, 10000);
    }
    nd_mutex_unlock(&c->send_lock);
    if (rc != 0)
        return -1;
    c->tx_bytes += len;
    nd_ws_broadcast_data(c->id, 1, data, len);
    conn_log_write(c, 1, (const unsigned char *)data, len);
    return 0;
}

int nd_conn_send_raw(nd_conn *c, const void *data, size_t len)
{
    int rc;
    if (!c || (!c->serial && c->fd < 0))
        return -1;
    nd_mutex_lock(&c->send_lock);
    if (c->ssh) {
        rc = nd_ssh_write((nd_ssh *)c->ssh, data, len);
    } else if (c->serial) {
        rc = nd_serial_write(c->serial, data, len);
    } else if (c->datagram) {
        rc = (c->last_peer.family == 0)
                 ? -1
                 : nd_net_sendto(c->fd, data, len, &c->last_peer, 10000);
    } else {
        rc = nd_net_send_all(c->fd, data, len, 10000);
    }
    nd_mutex_unlock(&c->send_lock);
    return rc;
}

void nd_conn_set_datagram(nd_conn *c, int on, int owns_fd)
{
    if (!c)
        return;
    c->datagram = on;
    c->owns_fd = owns_fd;
}

void nd_conn_set_peer(nd_conn *c, const nd_addr *peer)
{
    if (!c || !peer)
        return;
    nd_mutex_lock(&c->send_lock);
    c->last_peer = *peer;
    c->peer = *peer;
    nd_mutex_unlock(&c->send_lock);
}

void nd_conn_close(nd_conn *c)
{
    if (!c)
        return;
    atomic_fetch_add(&c->refs, 1); /* 关闭期间自保 */
    if (atomic_exchange(&c->closing, 1)) {
        nd_conn_put(c);
        return;
    }
    if (c->serial)
        nd_serial_cancel(c->serial);
    else if (c->fd >= 0)
        nd_net_shutdown(c->fd);
    if (c->started) {
        nd_thread_join(&c->th_rx);
        c->started = 0;
    } else {
        conn_teardown(c); /* 没有接收线程（UDP 服务端通道） */
    }
    nd_conn_put(c);
}

nd_conn *nd_conn_get(uint64_t id)
{
    nd_conn *p;
    conn_registry_init();
    nd_mutex_lock(&g_reg_lock);
    for (p = g_conns; p; p = p->next) {
        if (p->id == id) {
            atomic_fetch_add(&p->refs, 1);
            break;
        }
    }
    nd_mutex_unlock(&g_reg_lock);
    return p;
}

void nd_conn_put(nd_conn *c)
{
    if (!c)
        return;
    if (atomic_fetch_sub(&c->refs, 1) == 1) {
        if (c->service_id) {
            char ev[256], peer[96];
            nd_addr_format(&c->peer, 1, peer, sizeof(peer));
            snprintf(ev, sizeof(ev),
                     "{\"t\":\"conn_close\",\"svc\":%llu,\"conn\":%llu,\"proto\":\"%s\",\"peer\":\"%s\",\"ts\":%llu}",
                     (unsigned long long)c->service_id, (unsigned long long)c->id, c->proto, peer,
                     (unsigned long long)nd_wall_ms());
            nd_ws_event_json(ev);
        }
        nd_mutex_destroy(&c->send_lock);
        nd_mutex_destroy(&c->log_lock);
        free(c);
    }
}

size_t nd_conn_list(uint64_t service_id, uint64_t *ids, size_t cap)
{
    nd_conn *p;
    size_t n = 0;
    conn_registry_init();
    nd_mutex_lock(&g_reg_lock);
    for (p = g_conns; p && n < cap; p = p->next) {
        if (!service_id || p->service_id == service_id)
            ids[n++] = p->id;
    }
    nd_mutex_unlock(&g_reg_lock);
    return n;
}

int nd_conn_is_closed(nd_conn *c)
{
    return atomic_load(&c->closed);
}
