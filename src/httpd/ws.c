#include "httpd/ws.h"
#include "httpd/base64.h"
#include "httpd/sha1.h"
#include "core/fqueue.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "platform/netio.h"
#include "platform/platform.h"
#include "session/conn.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_PAYLOAD (8u * 1024u * 1024u)

typedef struct nd_ws_client {
    int             fd;
    uint64_t        sub_conn;
    nd_mutex        lock;
    nd_fqueue       q;
    nd_thread       th_wr;
    atomic_bool     closing;
    struct nd_ws_client *next;
} nd_ws_client;

static nd_mutex g_ws_lock;
static int g_ws_ready = 0;
static nd_ws_client *g_clients = NULL;

static void ws_registry_init(void)
{
    if (!g_ws_ready) {
        nd_mutex_init(&g_ws_lock);
        g_ws_ready = 1;
    }
}

static int ws_send_frame(nd_ws_client *c, int opcode, const void *data, size_t len)
{
    unsigned char hdr[14];
    size_t hlen = 2;

    hdr[0] = (unsigned char)(0x80 | (opcode & 0x0f));
    if (len < 126) {
        hdr[1] = (unsigned char)len;
    } else if (len <= 0xffff) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)len;
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (unsigned char)(len >> (56 - i * 8));
        hlen = 10;
    }

    if (nd_net_send_all(c->fd, hdr, hlen, 10000) != 0)
        return -1;
    if (len && nd_net_send_all(c->fd, data, len, 10000) != 0)
        return -1;
    return 0;
}

static void *ws_writer_thread(void *arg)
{
    nd_ws_client *c = (nd_ws_client *)arg;
    for (;;) {
        nd_frame *f = nd_fqueue_pop(&c->q, 500);
        if (!f) {
            if (atomic_load(&c->closing) || c->q.closed)
                break;
            continue;
        }
        if (f->len >= 1) {
            if (ws_send_frame(c, f->data[0], f->data + 1, f->len - 1) != 0) {
                atomic_store(&c->closing, 1);
                free(f);
                break;
            }
        }
        free(f);
    }
    return NULL;
}

static int ws_read_exact(nd_ws_client *c, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = nd_net_recv_some(c->fd, (char *)buf + got, len - got, 30000);
        if (n == -2) {
            if (atomic_load(&c->closing))
                return -1;
            continue; /* 空闲连接：超时后继续等 */
        }
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static int ws_handshake(int fd, const char *key)
{
    char concat[256];
    unsigned char sha[20];
    char accept[64];
    char resp[512];
    nd_sha1_ctx ctx;
    int n;

    if (!key || !key[0])
        return -1;
    snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
    nd_sha1_init(&ctx);
    nd_sha1_update(&ctx, concat, strlen(concat));
    nd_sha1_final(&ctx, sha);
    if (nd_base64_encode(sha, sizeof(sha), accept, sizeof(accept)) < 0)
        return -1;

    n = snprintf(resp, sizeof(resp),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n\r\n",
                 accept);
    return nd_net_send_all(fd, resp, (size_t)n, 10000);
}

static int ws_push_client(nd_ws_client *c, int opcode, const void *payload, size_t len)
{
    unsigned char *buf;
    size_t total = len + 1;
    if (total > WS_MAX_PAYLOAD + 1)
        return -1;
    buf = (unsigned char *)malloc(total);
    if (!buf)
        return -1;
    buf[0] = (unsigned char)opcode;
    if (len)
        memcpy(buf + 1, payload, len);
    if (nd_fqueue_push(&c->q, buf, total, 200) != 0) {
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
}

void nd_ws_broadcast_data(uint64_t conn_id, int dir, const void *data, size_t len)
{
    unsigned char *frame;
    uint64_t ts = nd_wall_ms();
    nd_ws_client *c;

    if (!g_ws_ready || len == 0)
        return;
    frame = (unsigned char *)malloc(len + 9);
    if (!frame)
        return;
    for (int i = 0; i < 8; i++)
        frame[i] = (unsigned char)(ts >> (i * 8));
    frame[8] = (unsigned char)dir;
    memcpy(frame + 9, data, len);

    nd_mutex_lock(&g_ws_lock);
    for (c = g_clients; c; c = c->next) {
        if (c->sub_conn == conn_id && !atomic_load(&c->closing))
            ws_push_client(c, 2, frame, len + 9);
    }
    nd_mutex_unlock(&g_ws_lock);
    free(frame);
}

void nd_ws_state_event(uint64_t conn_id, int state)
{
    char json[256];
    nd_ws_client *c;
    int n = snprintf(json, sizeof(json),
                     "{\"t\":\"state\",\"conn\":%llu,\"state\":\"%s\"}",
                     (unsigned long long)conn_id, state ? "open" : "closed");
    if (!g_ws_ready)
        return;
    nd_mutex_lock(&g_ws_lock);
    for (c = g_clients; c; c = c->next) {
        if (!atomic_load(&c->closing) && (c->sub_conn == conn_id || c->sub_conn == 0))
            ws_push_client(c, 1, json, (size_t)n);
    }
    nd_mutex_unlock(&g_ws_lock);
}

void nd_ws_event_json(const char *json)
{
    nd_ws_client *c;
    size_t n;
    if (!g_ws_ready || !json)
        return;
    n = strlen(json);
    nd_mutex_lock(&g_ws_lock);
    for (c = g_clients; c; c = c->next) {
        if (!atomic_load(&c->closing) && c->sub_conn == 0)
            ws_push_client(c, 1, json, n);
    }
    nd_mutex_unlock(&g_ws_lock);
}

int nd_ws_client_count(void)
{
    nd_ws_client *c;
    int n = 0;
    if (!g_ws_ready)
        return 0;
    nd_mutex_lock(&g_ws_lock);
    for (c = g_clients; c; c = c->next)
        n++;
    nd_mutex_unlock(&g_ws_lock);
    return n;
}

static void ws_handle_text(const char *data, size_t len)
{
    (void)data;
    (void)len;
    /* 目前只用二进制帧承载数据；文本帧保留给控制命令 */
}

int nd_ws_serve(int fd, const char *sec_ws_key, uint64_t sub_conn)
{
    nd_ws_client *c;
    unsigned char *msg = NULL;
    size_t msg_len = 0, msg_cap = 0;
    int msg_opcode = 0;
    int rc = 0;

    if (ws_handshake(fd, sec_ws_key) != 0) {
        nd_net_close(fd);
        return -1;
    }

    ws_registry_init();
    c = (nd_ws_client *)calloc(1, sizeof(*c));
    if (!c) {
        nd_net_close(fd);
        return -1;
    }
    c->fd = fd;
    c->sub_conn = sub_conn;
    atomic_init(&c->closing, 0);
    nd_mutex_init(&c->lock);
    nd_fqueue_init(&c->q, 8u * 1024u * 1024u);

    nd_mutex_lock(&g_ws_lock);
    c->next = g_clients;
    g_clients = c;
    nd_mutex_unlock(&g_ws_lock);

    if (nd_thread_start(&c->th_wr, ws_writer_thread, c, 256 * 1024) != 0) {
        nd_mutex_lock(&g_ws_lock);
        {
            nd_ws_client **pp = &g_clients;
            while (*pp && *pp != c)
                pp = &(*pp)->next;
            if (*pp)
                *pp = c->next;
        }
        nd_mutex_unlock(&g_ws_lock);
        nd_net_close(fd);
        nd_fqueue_destroy(&c->q);
        nd_mutex_destroy(&c->lock);
        free(c);
        return -1;
    }

    while (!atomic_load(&c->closing)) {
        unsigned char hdr[2];
        unsigned char ext[8];
        unsigned char mask[4];
        size_t len;
        int fin, opcode, masked;

        if (ws_read_exact(c, hdr, 2) != 0)
            break;
        fin = (hdr[0] & 0x80) ? 1 : 0;
        opcode = hdr[0] & 0x0f;
        masked = (hdr[1] & 0x80) ? 1 : 0;
        len = hdr[1] & 0x7f;
        if (len == 126) {
            if (ws_read_exact(c, ext, 2) != 0)
                break;
            len = ((size_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            if (ws_read_exact(c, ext, 8) != 0)
                break;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | ext[i];
        }
        if (!masked || len > WS_MAX_PAYLOAD) {
            rc = -1;
            break;
        }
        if (ws_read_exact(c, mask, 4) != 0)
            break;

        if (opcode >= 8) {
            unsigned char ctl[125];
            if (len > sizeof(ctl))
                break;
            if (len && ws_read_exact(c, ctl, len) != 0)
                break;
            for (size_t i = 0; i < len; i++)
                ctl[i] ^= mask[i % 4];
            if (opcode == 8) {
                ws_push_client(c, 8, ctl, len);
                break;
            }
            if (opcode == 9)
                ws_push_client(c, 10, ctl, len);
            continue;
        }

        if (opcode == 0) {
            if (!msg_opcode) {
                rc = -1;
                break;
            }
        } else {
            if (msg_opcode) {
                rc = -1;
                break;
            }
            msg_opcode = opcode;
        }

        if (msg_len + len + 1 > msg_cap) {
            size_t ncap = msg_len + len + 1;
            unsigned char *np = (unsigned char *)realloc(msg, ncap);
            if (!np)
                break;
            msg = np;
            msg_cap = ncap;
        }
        if (len) {
            if (ws_read_exact(c, msg + msg_len, len) != 0)
                break;
            for (size_t i = 0; i < len; i++)
                msg[msg_len + i] ^= mask[i % 4];
        }
        msg_len += len;
        msg[msg_len] = '\0';

        if (!fin)
            continue;

        if (msg_opcode == 1) {
            ws_handle_text((const char *)msg, msg_len);
        } else if (msg_opcode == 2 && c->sub_conn) {
            nd_conn *conn = nd_conn_get(c->sub_conn);
            if (conn) {
                nd_conn_send(conn, msg, msg_len);
                nd_conn_put(conn);
            }
        }
        msg_len = 0;
        msg_opcode = 0;
    }

    atomic_store(&c->closing, 1);
    nd_net_shutdown(fd);
    nd_fqueue_close(&c->q);
    nd_thread_join(&c->th_wr);

    nd_mutex_lock(&g_ws_lock);
    {
        nd_ws_client **pp = &g_clients;
        while (*pp && *pp != c)
            pp = &(*pp)->next;
        if (*pp)
            *pp = c->next;
    }
    nd_mutex_unlock(&g_ws_lock);

    nd_net_close(fd);
    nd_fqueue_destroy(&c->q);
    nd_mutex_destroy(&c->lock);
    free(c);
    free(msg);
    return rc;
}
