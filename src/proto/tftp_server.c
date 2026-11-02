#include "proto/tftp_server.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "platform/netio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_RRQ  1
#define OP_WRQ  2
#define OP_DATA 3
#define OP_ACK  4
#define OP_ERR  5
#define OP_OACK 6

typedef struct {
    int      listen_fd;
    nd_addr  local;
    nd_addr  peer;
    unsigned char req[1024];
    size_t   reqlen;
    nd_tftp_cfg cfg;
} tftp_job;

static uint16_t get16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(unsigned char *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static size_t put_str(unsigned char *p, size_t cap, const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > cap)
        n = cap;
    memcpy(p, s, n - 1);
    p[n - 1] = 0;
    return n;
}

static int map_path(const char *root, const char *name, char *out, size_t cap)
{
    char tmp[1024];
    char segs[32][128];
    int nseg = 0;
    char *save = NULL;
    char *tok;

    nd_strlcpy(tmp, name, sizeof(tmp));
    for (char *p = tmp; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
    tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (!strcmp(tok, ".") || !tok[0]) {
            /* skip */
        } else if (!strcmp(tok, "..")) {
            snprintf(out, cap, "..");
            return -1;
        } else {
            if (nseg >= 32)
                return -1;
            nd_strlcpy(segs[nseg++], tok, sizeof(segs[0]));
        }
        tok = strtok_r(NULL, "/", &save);
    }
    snprintf(out, cap, "%s", root);
    for (int i = 0; i < nseg; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "/%s", segs[i]);
    }
    return 0;
}

static int error_pkt(int fd, const nd_addr *peer, int code, const char *msg)
{
    unsigned char b[256];
    size_t n = 0;
    b[n++] = 0;
    b[n++] = OP_ERR;
    put16(b + n, (uint16_t)code);
    n += 2;
    n += put_str(b + n, sizeof(b) - n - 1, msg);
    return nd_net_sendto(fd, b, n, peer, 1000);
}

typedef struct {
    int       blksize;
    int       timeout_s;
    long long tsize;
    int       has_blksize;
    int       has_timeout;
    int       has_tsize;
} tftp_opts;

static void parse_req(const unsigned char *pkt, size_t len, char *name, size_t namecap,
                      tftp_opts *o)
{
    size_t pos = 2;
    char mode[32] = "";
    memset(o, 0, sizeof(*o));
    o->blksize = 512;
    o->timeout_s = 3;
    if (pos < len)
        nd_strlcpy(name, (const char *)pkt + pos, namecap);
    pos += strlen(name) + 1;
    if (pos < len)
        nd_strlcpy(mode, (const char *)pkt + pos, sizeof(mode));
    pos += strlen(mode) + 1;
    while (pos < len) {
        char key[64], val[64];
        nd_strlcpy(key, (const char *)pkt + pos, sizeof(key));
        pos += strlen(key) + 1;
        if (pos >= len)
            break;
        nd_strlcpy(val, (const char *)pkt + pos, sizeof(val));
        pos += strlen(val) + 1;
        if (!strcmp(key, "blksize")) {
            int v = atoi(val);
            if (v >= 8 && v <= 65464) {
                o->blksize = v;
                o->has_blksize = 1;
            }
        } else if (!strcmp(key, "timeout")) {
            int v = atoi(val);
            if (v >= 1 && v <= 60) {
                o->timeout_s = v;
                o->has_timeout = 1;
            }
        } else if (!strcmp(key, "tsize")) {
            o->tsize = atoll(val);
            o->has_tsize = 1;
        }
    }
}

static int send_oack(int fd, const nd_addr *peer, const tftp_opts *o, long long tsize)
{
    unsigned char b[256];
    size_t n = 0;
    char v[32];
    b[n++] = 0;
    b[n++] = OP_OACK;
    if (o->has_blksize) {
        n += put_str(b + n, sizeof(b) - n, "blksize");
        snprintf(v, sizeof(v), "%d", o->blksize);
        n += put_str(b + n, sizeof(b) - n, v);
    }
    if (o->has_timeout) {
        n += put_str(b + n, sizeof(b) - n, "timeout");
        snprintf(v, sizeof(v), "%d", o->timeout_s);
        n += put_str(b + n, sizeof(b) - n, v);
    }
    if (o->has_tsize) {
        n += put_str(b + n, sizeof(b) - n, "tsize");
        snprintf(v, sizeof(v), "%lld", tsize);
        n += put_str(b + n, sizeof(b) - n, v);
    }
    return nd_net_sendto(fd, b, n, peer, 1000);
}

static int recv_from(int fd, unsigned char *buf, size_t cap, nd_addr *peer,
                     int timeout_s, int *timed_out)
{
    int n = nd_net_recvfrom(fd, buf, cap, peer, timeout_s * 1000);
    *timed_out = n == -2;
    return n;
}

static void do_rrq(tftp_job *j, int fd, const char *file, const tftp_opts *o)
{
    FILE *fp = fopen(file, "rb");
    long long size = 0;
    unsigned char pkt[65536 + 4];
    unsigned char buf[65536];
    nd_addr peer = j->peer;
    int blk = 0;
    int use_oack = o->has_blksize || o->has_timeout || o->has_tsize;

    if (!fp) {
        error_pkt(fd, &peer, 1, "File not found");
        return;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (use_oack) {
        for (int retry = 0; retry < 5; retry++) {
            unsigned char ack[64];
            int to = 0;
            int n;
            send_oack(fd, &peer, o, size);
            n = recv_from(fd, ack, sizeof(ack), &peer, o->timeout_s, &to);
            if (n >= 4 && ack[1] == OP_ACK && get16(ack + 2) == 0)
                break;
            if (n >= 4 && ack[1] == OP_ERR)
                { fclose(fp); return; }
        }
    }

    /* 按块发送 */
    fseek(fp, 0, SEEK_SET);
    blk = 0;
    for (;;) {
        size_t n = fread(buf, 1, (size_t)o->blksize, fp);
        int acked = 0;
        blk++;
        pkt[0] = 0;
        pkt[1] = OP_DATA;
        put16(pkt + 2, (uint16_t)blk);
        memcpy(pkt + 4, buf, n);
        for (int retry = 0; retry < 6 && !acked; retry++) {
            unsigned char ack[64];
            int to = 0;
            int an;
            nd_net_sendto(fd, pkt, n + 4, &peer, 1000);
            an = recv_from(fd, ack, sizeof(ack), &peer, o->timeout_s, &to);
            if (an >= 4 && ack[1] == OP_ACK && get16(ack + 2) == (uint16_t)blk)
                acked = 1;
            else if (an >= 4 && ack[1] == OP_ERR)
                { fclose(fp); return; }
        }
        if (!acked)
            break;
        if (n < (size_t)o->blksize)
            break;
    }
    fclose(fp);
}

static void do_wrq(tftp_job *j, int fd, const char *file, const tftp_opts *o)
{
    unsigned char pkt[65536 + 4];
    unsigned char ack[4];
    nd_addr peer = j->peer;
    FILE *fp;
    uint16_t expect = 1;
    int use_oack = o->has_blksize || o->has_timeout || o->has_tsize;

    if (j->cfg.readonly) {
        error_pkt(fd, &peer, 2, "Access violation");
        return;
    }
    fp = fopen(file, "wb");
    if (!fp) {
        error_pkt(fd, &peer, 2, "Cannot create file");
        return;
    }

    if (use_oack)
        send_oack(fd, &peer, o, o->tsize);
    else {
        ack[0] = 0;
        ack[1] = OP_ACK;
        put16(ack + 2, 0);
        nd_net_sendto(fd, ack, 4, &peer, 1000);
    }

    for (;;) {
        int to = 0;
        int n = recv_from(fd, pkt, sizeof(pkt), &peer, o->timeout_s, &to);
        if (n < 0) {
            if (to)
                continue;
            break;
        }
        if (n >= 4 && pkt[1] == OP_ERR)
            break;
        if (n < 4 || pkt[1] != OP_DATA)
            continue;
        {
            uint16_t blk = get16(pkt + 2);
            size_t dlen = (size_t)n - 4;
            if (blk == (uint16_t)(expect - 1)) {
                ack[0] = 0;
                ack[1] = OP_ACK;
                put16(ack + 2, blk);
                nd_net_sendto(fd, ack, 4, &peer, 1000);
                continue;
            }
            if (blk != expect)
                continue;
            if (fwrite(pkt + 4, 1, dlen, fp) != dlen) {
                error_pkt(fd, &peer, 3, "Disk full");
                break;
            }
            ack[0] = 0;
            ack[1] = OP_ACK;
            put16(ack + 2, blk);
            nd_net_sendto(fd, ack, 4, &peer, 1000);
            if (dlen < (size_t)o->blksize)
                break;
            expect = (uint16_t)(expect + 1);
        }
    }
    fclose(fp);
}

static void *job_thread(void *arg)
{
    tftp_job *j = (tftp_job *)arg;
    int fd = nd_net_socket(j->local.family, SOCK_DGRAM);
    nd_addr bindaddr = j->local;
    char file[1200];
    char name[512];
    tftp_opts o;
    int op;

    bindaddr.port = 0;
    if (fd < 0 || nd_net_bind(fd, &bindaddr) != 0) {
        if (fd >= 0)
            nd_net_close(fd);
        free(j);
        return NULL;
    }

    op = j->req[1];
    parse_req(j->req, j->reqlen, name, sizeof(name), &o);
    if (map_path(j->cfg.root, name, file, sizeof(file)) != 0) {
        error_pkt(fd, &j->peer, 2, "Access violation");
    } else if (op == OP_RRQ) {
        do_rrq(j, fd, file, &o);
    } else {
        do_wrq(j, fd, file, &o);
    }
    nd_net_close(fd);
    free(j);
    return NULL;
}

int nd_tftp_server_start(int listen_fd, const nd_addr *local, const nd_addr *peer,
                         const unsigned char *pkt, size_t len, const nd_tftp_cfg *cfg)
{
    tftp_job *j;
    nd_thread th;
    int op;

    (void)listen_fd;
    if (len < 4)
        return -1;
    op = pkt[1];
    if (op != OP_RRQ && op != OP_WRQ)
        return -1;
    j = (tftp_job *)calloc(1, sizeof(*j));
    if (!j)
        return -1;
    j->local = *local;
    j->peer = *peer;
    j->cfg = *cfg;
    j->reqlen = len < sizeof(j->req) ? len : sizeof(j->req);
    memcpy(j->req, pkt, j->reqlen);
    if (nd_thread_start(&th, job_thread, j, 512 * 1024) != 0) {
        free(j);
        return -1;
    }
    nd_thread_detach(&th);
    return 0;
}
