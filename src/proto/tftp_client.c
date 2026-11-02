#include "proto/tftp_client.h"
#include "net/resolver.h"
#include "core/error.h"
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

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static size_t put_str(unsigned char *p, size_t cap, const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > cap)
        n = cap;
    memcpy(p, s, n - 1);
    p[n - 1] = 0;
    return n;
}

static int recv_pkt(int fd, unsigned char *buf, size_t cap, nd_addr *peer,
                    int timeout_s, char *err, size_t errlen)
{
    int n = nd_net_recvfrom(fd, buf, cap, peer, timeout_s * 1000);
    if (n == -2) {
        snprintf(err, errlen, "等待应答超时");
        return -2;
    }
    if (n < 0) {
        snprintf(err, errlen, "接收失败");
        return -1;
    }
    return n;
}

static int send_error(int fd, const nd_addr *peer, int code, const char *msg)
{
    unsigned char pkt[256];
    size_t n = 0;
    pkt[n++] = 0;
    pkt[n++] = OP_ERR;
    put16(pkt + n, (uint16_t)code);
    n += 2;
    n += put_str(pkt + n, sizeof(pkt) - n - 1, msg);
    return nd_net_sendto(fd, pkt, n, peer, 1000);
}

static int open_socket(int family, char *err, size_t errlen)
{
    int af = (family == AF_INET6) ? AF_INET6 : AF_INET;
    int fd = nd_net_socket(af, SOCK_DGRAM);
    nd_addr any;
    if (fd < 0) {
        snprintf(err, errlen, "创建 UDP 套接字失败");
        return -1;
    }
    memset(&any, 0, sizeof(any));
    any.family = af;
    any.port = 0;
    if (nd_net_bind(fd, &any) != 0) {
        nd_net_close(fd);
        snprintf(err, errlen, "绑定本地 UDP 端口失败");
        return -1;
    }
    return fd;
}

static int resolve_one(const char *host, uint16_t port, nd_family family,
                       nd_addr *out, char *err, size_t errlen)
{
    nd_addr addrs[8];
    size_t n = 0;
    if (nd_resolve(host, port, family, addrs, 8, &n, err, errlen) != 0)
        return -1;
    *out = addrs[0];
    return 0;
}

static int parse_oack(const unsigned char *pkg, int len, int *blksize, int *timeout_s)
{
    int pos = 2;
    while (pos < len) {
        const char *name = (const char *)pkg + pos;
        size_t nl = strlen(name);
        const char *val;
        pos += (int)nl + 1;
        if (pos >= len)
            break;
        val = (const char *)pkg + pos;
        pos += (int)strlen(val) + 1;
        if (!strcmp(name, "blksize"))
            *blksize = atoi(val);
        else if (!strcmp(name, "timeout"))
            *timeout_s = atoi(val);
    }
    if (*blksize < 8 || *blksize > ND_TFTP_MAX_BLKSIZE)
        *blksize = ND_TFTP_DEFAULT_BLKSIZE;
    if (*timeout_s < 1 || *timeout_s > 60)
        *timeout_s = 3;
    return 0;
}

int nd_tftp_download(const char *host, uint16_t port, nd_family family,
                     const char *remote, const char *local,
                     int blksize, int timeout_s, uint64_t *bytes,
                     char *err, size_t errlen)
{
    nd_addr server, peer;
    int fd = -1;
    unsigned char pkt[ND_TFTP_MAX_BLKSIZE + 4];
    unsigned char reqbuf[600];
    unsigned char ack[4];
    size_t reqlen = 0;
    FILE *fp = NULL;
    uint16_t expect = 1;
    int retries = 0;
    uint64_t total = 0;
    int rc = ND_E_IO;
    char optbuf[64];
    int negotiated_blk = ND_TFTP_DEFAULT_BLKSIZE;
    int negotiated_to = timeout_s > 0 ? timeout_s : 3;
    int first_len = 0;

    *bytes = 0;
    if (blksize < 8 || blksize > ND_TFTP_MAX_BLKSIZE)
        blksize = 1468;
    if (resolve_one(host, port, family, &server, err, errlen) != 0)
        return ND_E_RESOLVE;
    fd = open_socket(server.family, err, errlen);
    if (fd < 0)
        return ND_E_IO;

    pkt[reqlen++] = 0;
    pkt[reqlen++] = OP_RRQ;
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, remote);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "octet");
    snprintf(optbuf, sizeof(optbuf), "%d", blksize);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "blksize");
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, optbuf);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "timeout");
    snprintf(optbuf, sizeof(optbuf), "%d", timeout_s > 0 ? timeout_s : 3);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, optbuf);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "tsize");
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "0");
    memcpy(reqbuf, pkt, reqlen);

    if (nd_net_sendto(fd, reqbuf, reqlen, &server, 1000) != 0) {
        snprintf(err, errlen, "发送 RRQ 失败");
        goto done;
    }

    /* 等待 OACK 或 DATA1 或 ERROR */
    for (;;) {
        int n = recv_pkt(fd, pkt, sizeof(pkt), &peer, negotiated_to, err, errlen);
        if (n == -2) {
            if (++retries > 5)
                goto done;
            nd_net_sendto(fd, reqbuf, reqlen, &server, 1000);
            continue;
        }
        if (n < 4)
            continue;
        if (pkt[1] == OP_ERR) {
            snprintf(err, errlen, "服务器错误: %s", (char *)pkt + 4);
            rc = ND_E_PROTO;
            goto done;
        }
        if (pkt[1] == OP_OACK) {
            parse_oack(pkt, n, &negotiated_blk, &negotiated_to);
            ack[0] = 0;
            ack[1] = OP_ACK;
            put16(ack + 2, 0);
            nd_net_sendto(fd, ack, 4, &peer, 1000);
            break;
        }
        if (pkt[1] == OP_DATA && get16(pkt + 2) == 1) {
            negotiated_blk = ND_TFTP_DEFAULT_BLKSIZE;
            expect = 1;
            first_len = n;
            break;
        }
    }

    fp = fopen(local, "wb");
    if (!fp) {
        snprintf(err, errlen, "无法写入本地文件: %s", local);
        goto done;
    }

    /* 若首个包里已经带了 DATA1，先处理 */
    if (pkt[1] == OP_DATA && get16(pkt + 2) == 1) {
        size_t dlen = (size_t)first_len - 4;
        if (fwrite(pkt + 4, 1, dlen, fp) != dlen) {
            snprintf(err, errlen, "写入失败");
            goto done;
        }
        total += dlen;
        ack[0] = 0;
        ack[1] = OP_ACK;
        put16(ack + 2, 1);
        nd_net_sendto(fd, ack, 4, &peer, 1000);
        if (dlen < (size_t)negotiated_blk) {
            rc = ND_OK;
            goto done;
        }
        expect = 2;
    }

    for (;;) {
        int n = recv_pkt(fd, pkt, sizeof(pkt), &peer, negotiated_to, err, errlen);
        if (n == -2) {
            if (++retries > 6) {
                snprintf(err, errlen, "传输超时（已收 %llu 字节）", (unsigned long long)total);
                goto done;
            }
            /* 重发上一个 ACK，触发服务器重传 */
            ack[0] = 0;
            ack[1] = OP_ACK;
            put16(ack + 2, (uint16_t)(expect - 1));
            nd_net_sendto(fd, ack, 4, &peer, 1000);
            continue;
        }
        if (n < 4)
            continue;
        if (pkt[1] == OP_ERR) {
            snprintf(err, errlen, "服务器错误: %s", (char *)pkt + 4);
            rc = ND_E_PROTO;
            goto done;
        }
        if (pkt[1] != OP_DATA)
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
                snprintf(err, errlen, "写入失败");
                goto done;
            }
            total += dlen;
            ack[0] = 0;
            ack[1] = OP_ACK;
            put16(ack + 2, blk);
            nd_net_sendto(fd, ack, 4, &peer, 1000);
            retries = 0;
            if (dlen < (size_t)negotiated_blk) {
                rc = ND_OK;
                goto done;
            }
            expect = (uint16_t)(expect + 1);
        }
    }

done:
    if (fp)
        fclose(fp);
    if (fd >= 0)
        nd_net_close(fd);
    *bytes = total;
    return rc;
}

int nd_tftp_upload(const char *host, uint16_t port, nd_family family,
                   const char *local, const char *remote,
                   int blksize, int timeout_s, uint64_t *bytes,
                   char *err, size_t errlen)
{
    nd_addr server, peer;
    int fd = -1;
    unsigned char pkt[ND_TFTP_MAX_BLKSIZE + 4];
    unsigned char reqbuf[600];
    unsigned char rbuf[512];
    size_t reqlen = 0;
    FILE *fp = NULL;
    uint16_t blk = 0;
    int retries = 0;
    uint64_t total = 0;
    int rc = ND_E_IO;
    int negotiated_blk = ND_TFTP_DEFAULT_BLKSIZE;
    int negotiated_to = timeout_s > 0 ? timeout_s : 3;
    int have_peer = 0;
    char optbuf[64];

    *bytes = 0;
    if (blksize < 8 || blksize > ND_TFTP_MAX_BLKSIZE)
        blksize = 1468;
    if (resolve_one(host, port, family, &server, err, errlen) != 0)
        return ND_E_RESOLVE;
    fd = open_socket(server.family, err, errlen);
    if (fd < 0)
        return ND_E_IO;

    pkt[reqlen++] = 0;
    pkt[reqlen++] = OP_WRQ;
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, remote);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "octet");
    snprintf(optbuf, sizeof(optbuf), "%d", blksize);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "blksize");
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, optbuf);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, "timeout");
    snprintf(optbuf, sizeof(optbuf), "%d", timeout_s > 0 ? timeout_s : 3);
    reqlen += put_str(pkt + reqlen, sizeof(pkt) - reqlen, optbuf);
    memcpy(reqbuf, pkt, reqlen);

    fp = fopen(local, "rb");
    if (!fp) {
        snprintf(err, errlen, "无法读取本地文件: %s", local);
        goto done;
    }

    if (nd_net_sendto(fd, reqbuf, reqlen, &server, 1000) != 0) {
        snprintf(err, errlen, "发送 WRQ 失败");
        goto done;
    }

    for (;;) {
        int n = recv_pkt(fd, rbuf, sizeof(rbuf), &peer, negotiated_to, err, errlen);
        if (n == -2) {
            if (++retries > 5)
                goto done;
            nd_net_sendto(fd, reqbuf, reqlen, &server, 1000);
            continue;
        }
        if (n < 4)
            continue;
        if (rbuf[1] == OP_ERR) {
            snprintf(err, errlen, "服务器错误: %s", (char *)rbuf + 4);
            rc = ND_E_PROTO;
            goto done;
        }
        if (rbuf[1] == OP_OACK) {
            parse_oack(rbuf, n, &negotiated_blk, &negotiated_to);
            have_peer = 1;
            break;
        }
        if (rbuf[1] == OP_ACK && get16(rbuf + 2) == 0) {
            negotiated_blk = ND_TFTP_DEFAULT_BLKSIZE;
            have_peer = 1;
            break;
        }
    }
    (void)have_peer;

    for (;;) {
        size_t n = fread(pkt + 4, 1, (size_t)negotiated_blk, fp);
        blk++;
        pkt[0] = 0;
        pkt[1] = OP_DATA;
        put16(pkt + 2, blk);
        for (retries = 0; retries < 6; retries++) {
            int r;
            nd_net_sendto(fd, pkt, n + 4, &peer, 1000);
            r = recv_pkt(fd, rbuf, sizeof(rbuf), &peer, negotiated_to, err, errlen);
            if (r >= 4) {
                if (rbuf[1] == OP_ACK && get16(rbuf + 2) == blk)
                    break;
                if (rbuf[1] == OP_ERR) {
                    snprintf(err, errlen, "服务器错误: %s", (char *)rbuf + 4);
                    goto done;
                }
            }
        }
        if (retries >= 6) {
            snprintf(err, errlen, "块 %u 重传失败", (unsigned)blk);
            goto done;
        }
        total += n;
        if (n < (size_t)negotiated_blk) {
            rc = ND_OK;
            goto done;
        }
    }

done:
    if (fp)
        fclose(fp);
    if (fd >= 0)
        nd_net_close(fd);
    *bytes = total;
    return rc;
}
