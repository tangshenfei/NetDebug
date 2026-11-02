#include "proto/http_client.h"
#include "net/addr.h"
#include "net/resolver.h"
#include "net/url.h"
#include "platform/netio.h"
#include "core/error.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif

/* ---------------- 动态缓冲 ---------------- */
typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} sbuf;

static int sbuf_reserve(sbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 0;
    {
        size_t ncap = b->cap ? b->cap : 256;
        while (ncap < b->len + extra + 1)
            ncap *= 2;
        char *np = (char *)realloc(b->p, ncap);
        if (!np)
            return -1;
        b->p = np;
        b->cap = ncap;
    }
    return 0;
}

static int sbuf_put(sbuf *b, const void *data, size_t len)
{
    if (sbuf_reserve(b, len) != 0)
        return -1;
    memcpy(b->p + b->len, data, len);
    b->len += len;
    b->p[b->len] = '\0';
    return 0;
}

static int sbuf_puts(sbuf *b, const char *s)
{
    return sbuf_put(b, s, strlen(s));
}

static int sbuf_printf(sbuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= sizeof(tmp))
        n = (int)sizeof(tmp) - 1;
    return sbuf_put(b, tmp, (size_t)n);
}

/* ---------------- 读取器 ---------------- */
typedef struct {
    int      fd;
    unsigned char buf[16384];
    size_t   pos, len;
    uint64_t deadline;
    int      eof;
    int      timed_out;
} reader;

static int rd_fill(reader *r)
{
    if (r->pos < r->len)
        return 1;
    for (;;) {
        uint64_t now = nd_monotonic_ms();
        int wait;
        nd_pollfd p;
        int pr;
        int n;

        if (now >= r->deadline) {
            r->timed_out = 1;
            return -1;
        }
        wait = (int)(r->deadline - now);
        if (wait > 1000)
            wait = 1000;
        p.fd = r->fd;
        p.events = ND_POLLIN;
        p.revents = 0;
        pr = nd_net_poll(&p, 1, wait);
        if (pr == 0)
            continue;
        if (pr < 0)
            return -1;

        n = nd_net_recv_some(r->fd, r->buf, sizeof(r->buf), 0);
        if (n > 0) {
            r->pos = 0;
            r->len = (size_t)n;
            return 1;
        }
        if (n == -2)
            continue;
        if (n == 0)
            r->eof = 1;
        return -1;
    }
}

static int rd_get(reader *r, void *out, size_t n)
{
    unsigned char *p = (unsigned char *)out;
    size_t got = 0;
    while (got < n) {
        size_t avail = r->len - r->pos;
        size_t take;
        if (avail == 0) {
            if (rd_fill(r) != 1)
                return -1;
            avail = r->len - r->pos;
        }
        take = n - got;
        if (take > avail)
            take = avail;
        memcpy(p + got, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    return 0;
}

static int rd_byte(reader *r, unsigned char *c)
{
    return rd_get(r, c, 1);
}

static int rd_line(reader *r, char *out, size_t cap)
{
    size_t n = 0;
    unsigned char c;
    for (;;) {
        if (rd_byte(r, &c) != 0)
            return -1;
        if (c == '\n') {
            if (n > 0 && out[n - 1] == '\r')
                n--;
            out[n] = '\0';
            return 0;
        }
        if (n + 1 >= cap) {
            out[cap - 1] = '\0';
            /* 丢弃剩余字符直到换行 */
            while (rd_byte(r, &c) == 0 && c != '\n')
                ;
            return 0;
        }
        out[n++] = (char)c;
    }
}

/* ---------------- 响应头 ---------------- */
static const char *hdr_get(const nd_http_result *r, const char *name)
{
    for (int i = 0; i < r->n_headers; i++) {
#ifdef _WIN32
        if (_stricmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
#else
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
#endif
    }
    return NULL;
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!hay)
        return 0;
    for (; *hay; hay++) {
#ifdef _WIN32
        if (_strnicmp(hay, needle, nl) == 0)
            return 1;
#else
        if (strncasecmp(hay, needle, nl) == 0)
            return 1;
#endif
    }
    return 0;
}

static int parse_head(char *head, size_t len, nd_http_result *r)
{
    char *p = head;
    char *end = head + len;
    char *line_end;
    char line[2048];

    /* 第一行：HTTP/1.1 200 OK */
    line_end = strstr(p, "\r\n");
    if (!line_end)
        return -1;
    if ((size_t)(line_end - p) >= sizeof(line))
        return -1;
    memcpy(line, p, (size_t)(line_end - p));
    line[line_end - p] = '\0';
    {
        char ver[16] = "";
        int status = 0;
        char reason[128] = "";
        if (sscanf(line, "HTTP/%15s %d %127[^\r\n]", ver, &status, reason) < 2)
            return -1;
        nd_strlcpy(r->version, ver, sizeof(r->version));
        r->status = status;
        nd_strlcpy(r->reason, reason, sizeof(r->reason));
    }
    p = line_end + 2;

    while (p < end) {
        char *colon;
        line_end = strstr(p, "\r\n");
        if (!line_end)
            break;
        if (line_end == p)
            break;
        if ((size_t)(line_end - p) >= sizeof(line))
            break;
        memcpy(line, p, (size_t)(line_end - p));
        line[line_end - p] = '\0';
        colon = strchr(line, ':');
        if (colon && r->n_headers < ND_HTTP_MAX_HEADERS) {
            nd_http_resp_hdr *h = &r->headers[r->n_headers++];
            *colon = '\0';
            nd_strlcpy(h->name, line, sizeof(h->name));
            nd_strlcpy(h->value, colon + 1, sizeof(h->value));
            nd_trim(h->value);
        }
        p = line_end + 2;
    }

    {
        const char *ce = hdr_get(r, "content-encoding");
        if (ce)
            nd_strlcpy(r->content_encoding, ce, sizeof(r->content_encoding));
    }
    return 0;
}

/* ---------------- 请求 ---------------- */
static int build_head(const nd_url *u, const char *method,
                      const nd_http_req_hdr *hdrs, int n_hdrs,
                      size_t body_len, sbuf *out)
{
    int def_port = u->tls ? 443 : 80;
    int has_host = 0, has_clen = 0, has_conn = 0, has_ua = 0, has_accept = 0;
    char hosthdr[512];

    if (strchr(u->host, ':')) {
        if (u->port == def_port)
            snprintf(hosthdr, sizeof(hosthdr), "[%s]", u->host);
        else
            snprintf(hosthdr, sizeof(hosthdr), "[%s]:%u", u->host, (unsigned)u->port);
    } else {
        if (u->port == def_port)
            snprintf(hosthdr, sizeof(hosthdr), "%s", u->host);
        else
            snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u->host, (unsigned)u->port);
    }

    if (sbuf_printf(out, "%s %s HTTP/1.1\r\n", method, u->path) != 0)
        return -1;
    for (int i = 0; i < n_hdrs; i++) {
        if (!strcasecmp(hdrs[i].name, "host"))
            has_host = 1;
        if (!strcasecmp(hdrs[i].name, "content-length"))
            has_clen = 1;
        if (!strcasecmp(hdrs[i].name, "connection"))
            has_conn = 1;
        if (!strcasecmp(hdrs[i].name, "user-agent"))
            has_ua = 1;
        if (!strcasecmp(hdrs[i].name, "accept"))
            has_accept = 1;
    }
    if (!has_host && sbuf_printf(out, "Host: %s\r\n", hosthdr) != 0)
        return -1;
    if (!has_ua && sbuf_puts(out, "User-Agent: NetDebug/0.1\r\n") != 0)
        return -1;
    if (!has_accept && sbuf_puts(out, "Accept: */*\r\n") != 0)
        return -1;
    if (!has_conn && sbuf_puts(out, "Connection: close\r\n") != 0)
        return -1;

    for (int i = 0; i < n_hdrs; i++) {
        if (!hdrs[i].name[0])
            continue;
        if (sbuf_printf(out, "%s: %s\r\n", hdrs[i].name, hdrs[i].value) != 0)
            return -1;
    }
    if (!has_clen && (body_len > 0 || !strcasecmp(method, "POST") ||
                      !strcasecmp(method, "PUT") || !strcasecmp(method, "PATCH"))) {
        if (sbuf_printf(out, "Content-Length: %zu\r\n", body_len) != 0)
            return -1;
    }
    return sbuf_puts(out, "\r\n");
}

static int read_body(reader *rd, const nd_http_result *r, const char *method,
                     char **out_body, size_t *out_len, int *truncated)
{
    const char *te = hdr_get(r, "transfer-encoding");
    const char *cl = hdr_get(r, "content-length");
    char *body = NULL;
    size_t len = 0, cap = 0;
    int chunked = contains_ci(te, "chunked");
    long long want = -1;
    int rc = 0;

    *out_body = NULL;
    *out_len = 0;
    *truncated = 0;

    if (r->status == 204 || r->status == 304 || !strcasecmp(method, "HEAD"))
        return 0;

    if (!chunked && cl) {
        want = atoll(cl);
        if (want < 0)
            want = -1;
    }

#define ENSURE(n)                                                              \
    do {                                                                       \
        size_t need_ = (size_t)(n);                                            \
        if (need_ > ND_HTTP_MAX_BODY) {                                        \
            *truncated = 1;                                                    \
            goto done;                                                         \
        }                                                                      \
        if (cap < need_) {                                                     \
            size_t ncap_ = cap ? cap : 4096;                                   \
            char *np_;                                                         \
            while (ncap_ < need_)                                              \
                ncap_ *= 2;                                                    \
            np_ = (char *)realloc(body, ncap_);                                \
            if (!np_) {                                                        \
                rc = -1;                                                       \
                goto done;                                                     \
            }                                                                  \
            body = np_;                                                        \
            cap = ncap_;                                                       \
        }                                                                      \
    } while (0)

    if (chunked) {
        for (;;) {
            char line[256];
            unsigned long long sz;
            if (rd_line(rd, line, sizeof(line)) != 0) {
                rc = rd->timed_out ? -2 : -1;
                break;
            }
            {
                char *semi = strchr(line, ';');
                if (semi)
                    *semi = '\0';
            }
            sz = strtoull(line, NULL, 16);
            if (sz == 0) {
                while (rd_line(rd, line, sizeof(line)) == 0 && line[0])
                    ;
                break;
            }
            if (len + sz > ND_HTTP_MAX_BODY) {
                *truncated = 1;
                break;
            }
            ENSURE(len + (size_t)sz);
            if (rd_get(rd, body + len, (size_t)sz) != 0) {
                rc = -2;
                break;
            }
            len += (size_t)sz;
            {
                char crlf[2];
                if (rd_get(rd, crlf, 2) != 0) {
                    rc = -2;
                    break;
                }
            }
        }
    } else if (want >= 0) {
        size_t need = (size_t)want;
        size_t got = 0;
        while (got < need) {
            size_t take = need - got;
            if (take > 16384)
                take = 16384;
            ENSURE(len + take);
            if (rd_get(rd, body + len, take) != 0) {
                *truncated = 1;
                break;
            }
            len += take;
            got += take;
            if (len >= ND_HTTP_MAX_BODY) {
                *truncated = 1;
                break;
            }
        }
    } else {
        for (;;) {
            size_t avail = rd->len - rd->pos;
            if (avail == 0) {
                if (rd_fill(rd) != 1) {
                    if (rd->timed_out)
                        *truncated = 1;
                    break;
                }
                avail = rd->len - rd->pos;
            }
            if (len + avail > ND_HTTP_MAX_BODY) {
                avail = ND_HTTP_MAX_BODY - len;
                *truncated = 1;
            }
            ENSURE(len + avail);
            memcpy(body + len, rd->buf + rd->pos, avail);
            len += avail;
            rd->pos += avail;
            if (*truncated)
                break;
        }
    }

done:
    if (body && len + 1 < cap)
        body[len] = '\0';
    *out_body = body;
    *out_len = len;
    return rc;
#undef ENSURE
}

static int http_once(const nd_url *u, const char *method,
                     const nd_http_req_hdr *hdrs, int n_hdrs,
                     const void *body, size_t body_len,
                     int timeout_ms, nd_http_result *r,
                     char *err, size_t errlen)
{
    nd_addr addrs[16];
    size_t n = 0;
    int fd = -1;
    int sys = 0;
    uint64_t t0, t1;
    sbuf req = {0};
    int rc = ND_E_CONNECT_ALL;

    if (nd_resolve(u->host, u->port, ND_FAM_DUAL, addrs, 16, &n, err, errlen) != 0)
        return ND_E_RESOLVE;

    t0 = nd_monotonic_ms();
    for (size_t i = 0; i < n; i++) {
        fd = nd_net_socket(addrs[i].family, SOCK_STREAM);
        if (fd < 0)
            continue;
        if (nd_net_connect(fd, &addrs[i], 5000, &sys) == 0) {
            r->connect_ms = nd_monotonic_ms() - t0;
            break;
        }
        nd_net_close(fd);
        fd = -1;
    }
    if (fd < 0) {
        snprintf(err, errlen, "连接失败（尝试 %zu 个地址）", n);
        return ND_E_CONNECT_ALL;
    }

    if (build_head(u, method, hdrs, n_hdrs, body_len, &req) != 0) {
        nd_net_close(fd);
        free(req.p);
        snprintf(err, errlen, "构造请求失败");
        return ND_E_INTERNAL;
    }
    r->req_head = req.p;
    r->req_head_len = req.len;

    if (nd_net_send_all(fd, req.p, req.len, 10000) != 0 ||
        (body_len && nd_net_send_all(fd, body, body_len, 10000) != 0)) {
        nd_net_close(fd);
        snprintf(err, errlen, "发送请求失败");
        return ND_E_IO;
    }

    {
        reader rd;
        sbuf head = {0};
        uint64_t deadline = nd_monotonic_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 15000);

        memset(&rd, 0, sizeof(rd));
        rd.fd = fd;
        rd.deadline = deadline;

        t1 = nd_monotonic_ms();
        for (;;) {
            unsigned char c;
            if (rd_byte(&rd, &c) != 0) {
                free(head.p);
                nd_net_close(fd);
                snprintf(err, errlen, rd.timed_out ? "等待响应超时" : "连接被关闭");
                return ND_E_CONNECT_TIMEOUT;
            }
            if (sbuf_put(&head, &c, 1) != 0) {
                free(head.p);
                nd_net_close(fd);
                return ND_E_INTERNAL;
            }
            if (head.len >= 4 && !memcmp(head.p + head.len - 4, "\r\n\r\n", 4))
                break;
            if (head.len > 64 * 1024) {
                free(head.p);
                nd_net_close(fd);
                snprintf(err, errlen, "响应头过大");
                return ND_E_PROTO;
            }
        }
        r->first_byte_ms = nd_monotonic_ms() - t1;

        {
            char *copy = (char *)malloc(head.len + 1);
            if (!copy) {
                free(head.p);
                nd_net_close(fd);
                return ND_E_INTERNAL;
            }
            memcpy(copy, head.p, head.len);
            copy[head.len] = '\0';
            if (parse_head(copy, head.len, r) != 0) {
                free(copy);
                free(head.p);
                nd_net_close(fd);
                snprintf(err, errlen, "响应头解析失败");
                return ND_E_PROTO;
            }
            free(copy);
        }
        r->resp_head = head.p;
        r->resp_head_len = head.len;

        rc = read_body(&rd, r, method, &r->body, &r->body_len, &r->truncated);
        if (rc == -2)
            snprintf(err, errlen, "读取响应体超时（已收 %zu 字节）", r->body_len);
        else if (rc == -1)
            snprintf(err, errlen, "读取响应体失败");
        else
            rc = ND_OK;
    }

    nd_net_close(fd);
    r->total_ms = nd_monotonic_ms() - t0;
    return rc;
}

void nd_http_result_free(nd_http_result *r)
{
    if (!r)
        return;
    free(r->body);
    free(r->req_head);
    free(r->resp_head);
    memset(r, 0, sizeof(*r));
}

int nd_http_do(const char *method, const char *url,
               const nd_http_req_hdr *hdrs, int n_hdrs,
               const void *body, size_t body_len,
               int timeout_ms, int follow_redirects,
               nd_http_result *out, char *err, size_t errlen)
{
    nd_url u;
    const char *cur_method = method;
    const void *cur_body = body;
    size_t cur_len = body_len;

    memset(out, 0, sizeof(*out));
    if (nd_url_parse(url, &u, err, errlen) != ND_OK)
        return ND_E_PARAM;
    if (u.tls) {
        snprintf(err, errlen, "HTTPS 需要 TLS 支持，当前版本尚未启用");
        return ND_E_PROTO;
    }

    for (int depth = 0; depth <= 5; depth++) {
        int rc = http_once(&u, cur_method, hdrs, n_hdrs, cur_body, cur_len,
                           timeout_ms, out, err, errlen);
        if (rc != ND_OK)
            return rc;
        out->redirects = depth;
        if (u.port == 80)
            snprintf(out->final_url, sizeof(out->final_url), "http://%s%s", u.host, u.path);
        else
            snprintf(out->final_url, sizeof(out->final_url), "http://%s:%u%s",
                     u.host, (unsigned)u.port, u.path);

        if (!follow_redirects)
            return ND_OK;
        if (!(out->status == 301 || out->status == 302 || out->status == 303 ||
              out->status == 307 || out->status == 308))
            return ND_OK;

        {
            const char *loc = hdr_get(out, "location");
            char location[2048];
            nd_url next;
            char rerr[256] = "";
            int status = out->status;

            if (!loc)
                return ND_OK;
            nd_strlcpy(location, loc, sizeof(location));
            if (nd_url_resolve(&u, location, &next, rerr, sizeof(rerr)) != ND_OK) {
                snprintf(err, errlen, "重定向地址无效: %s", rerr);
                return ND_E_PROTO;
            }
            if (next.tls) {
                snprintf(err, errlen, "重定向到 HTTPS，当前版本不支持");
                return ND_E_PROTO;
            }

            if (status == 303 || ((status == 301 || status == 302) &&
                                  !strcasecmp(cur_method, "POST"))) {
                cur_method = "GET";
                cur_body = NULL;
                cur_len = 0;
            }
            u = next;
            nd_http_result_free(out);
        }
    }

    snprintf(err, errlen, "重定向次数过多");
    return ND_E_PROTO;
}
