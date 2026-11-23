#include "proto/syslog_server.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "httpd/ws.h"
#include "net/addr.h"
#include "platform/netio.h"
#include "platform/platform.h"

#include "cJSON.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SYSLOG_MAX_FRAME (1024 * 1024) /* 单帧上限 1MB，超过视为垃圾流重新同步 */

static const char *SEV_NAME[8] = {
    "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"
};

static const char *FAC_NAME[24] = {
    "kern", "user", "mail", "daemon", "auth", "syslog", "lpr", "news",
    "uucp", "cron", "authpriv", "ftp", "ntp", "audit", "alert", "clock",
    "local0", "local1", "local2", "local3",
    "local4", "local5", "local6", "local7"
};

const char *nd_syslog_severity_name(int sev)
{
    if (sev < 0 || sev > 7)
        return "unknown";
    return SEV_NAME[sev];
}

const char *nd_syslog_facility_name(int fac)
{
    if (fac < 0 || fac > 23)
        return "unknown";
    return FAC_NAME[fac];
}

/* 在 raw[len] 中找字符串直到空格/结尾；返回长度（不含空格） */
static size_t tok_len(const char *p, size_t len)
{
    size_t n = 0;
    while (n < len && p[n] != ' ' && p[n] != '\0')
        n++;
    return n;
}

static int digits(const char *p, size_t n)
{
    if (n == 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return 0;
    return 1;
}

int nd_syslog_parse(const char *raw, size_t len, nd_syslog_msg *m)
{
    const char *p = raw;
    size_t left = len;

    memset(m, 0, sizeof(*m));
    m->facility = -1;
    m->severity = -1;

    while (left && (*p == ' ' || *p == '\r' || *p == '\n')) {
        p++;
        left--;
    }
    if (!left)
        return -1;

    /* PRI：合法则解析级别；非法（超范围）整条按原文处理 */
    if (p[0] == '<') {
        const char *gt = (const char *)memchr(p + 1, '>', left - 1);
        if (gt) {
            size_t plen = (size_t)(gt - (p + 1));
            if (plen > 0 && plen <= 5 && digits(p + 1, plen)) {
                int pri = 0;
                for (size_t i = 0; i < plen; i++)
                    pri = pri * 10 + (p[1 + i] - '0');
                if (pri >= 0 && pri <= 191) {
                    m->facility = pri / 8;
                    m->severity = pri % 8;
                    snprintf(m->pri, sizeof(m->pri), "<%d>", pri);
                } else {
                    snprintf(m->body, sizeof(m->body), "%.*s",
                             (int)(len > sizeof(m->body) - 1 ? sizeof(m->body) - 1 : len),
                             raw);
                    return 0;
                }
            }
            p = gt + 1;
            left = len - (size_t)(p - raw);
            while (left && *p == ' ') {
                p++;
                left--;
            }
        }
    }
    if (!left)
        return m->severity >= 0 ? 0 : -1;

    /* RFC 5424：version + space 开头 */
    {
        size_t vl = tok_len(p, left);
        if (vl && p[vl] == ' ' && digits(p, vl) && vl <= 2) {
            const char *q = p + vl + 1;
            size_t ql = left - vl - 1;
            size_t field = 0;
            nd_strlcpy(m->version, "1", sizeof(m->version));
            while (ql && field < 5) {
                size_t fl = tok_len(q, ql);
                if (field == 0)
                    snprintf(m->ts, sizeof(m->ts), "%.*s", (int)fl, q);
                else if (field == 1)
                    snprintf(m->host, sizeof(m->host), "%.*s", (int)fl, q);
                else if (field == 2)
                    snprintf(m->tag, sizeof(m->tag), "%.*s", (int)fl, q);
                field++;
                q += fl;
                ql -= fl;
                if (ql) {
                    q++;
                    ql--;
                }
            }
            /* 剩余部分从 structured-data 后取 msg */
            if (ql && *q == '[') {
                const char *sd = q;
                size_t sdl = ql;
                while (sdl && *sd == '[') {
                    const char *cl = (const char *)memchr(sd, ']', sdl);
                    if (!cl)
                        break;
                    size_t skip = (size_t)(cl - sd) + 1;
                    sd += skip;
                    sdl -= skip;
                    if (sdl) {
                        sd++;
                        sdl--;
                    }
                }
                snprintf(m->body, sizeof(m->body), "%.*s",
                         (int)(sdl > sizeof(m->body) - 1 ? sizeof(m->body) - 1 : sdl), sd);
            }
            return 0;
        }
    }

    /* RFC 3164：可选 "Mmm dd hh:mm:ss "，然后 host tag[pid]: body */
    {
        const char *q = p;
        size_t ql = left;
        /* 尝试跳过时间戳：形如 "Oct 11 22:14:15 " */
        if (ql > 16 && p[3] == ' ' && digits(p + 4, 2) && p[6] == ' ' &&
            digits(p + 7, 2) && p[9] == ':' && digits(p + 10, 2) &&
            p[12] == ':' && digits(p + 13, 2) && p[15] == ' ') {
            size_t tl = 16;
            snprintf(m->ts, sizeof(m->ts), "%.*s", 15, p);
            q = p + tl;
            ql = left - tl;
        }
        /* host：到空格 */
        {
            size_t hl = tok_len(q, ql);
            if (hl && ql > hl && q[hl] == ' ') {
                snprintf(m->host, sizeof(m->host), "%.*s", (int)hl, q);
                q += hl + 1;
                ql -= hl + 1;
            }
        }
        /* tag[pid]: body —— tag 到冒号；没有冒号时整段留给 body */
        {
            const char *colon = (const char *)memchr(q, ':', ql);
            const char *sp = (const char *)memchr(q, ' ', ql);
            if (colon && (!sp || colon < sp) && colon > q) {
                size_t tl2 = (size_t)(colon - q);
                snprintf(m->tag, sizeof(m->tag), "%.*s", (int)tl2, q);
                q = colon + 1;
                ql -= tl2 + 1;
                while (ql && *q == ' ') {
                    q++;
                    ql--;
                }
            }
            snprintf(m->body, sizeof(m->body), "%.*s",
                     (int)(ql > sizeof(m->body) - 1 ? sizeof(m->body) - 1 : ql), q);
        }
    }

    return 0;
}

/* ---------- 落盘 ---------- */

static void syslog_make_path(nd_service *s, char *path, size_t cap, const char *day)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%slogs", nd_config_dir());
    nd_mkdir_p(dir);
    snprintf(path, cap, "%s/syslog-%llu-%s.log", dir,
             (unsigned long long)s->id, day);
}

void nd_syslog_log_open(nd_service *s)
{
    char path[600];
    char day[16];
    time_t now = time(NULL);
    struct tm tm_now;

    if (!s || !s->syslog_lock_init)
        return;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(day, sizeof(day), "%Y%m%d", &tm_now);

    nd_mutex_lock(&s->syslog_lock);
    if (s->syslog_file) {
        fflush(s->syslog_file);
        fclose(s->syslog_file);
        s->syslog_file = NULL;
    }
    nd_strlcpy(s->syslog_day, day, sizeof(s->syslog_day));
    syslog_make_path(s, path, sizeof(path), day);
    s->syslog_file = fopen(path, "ab");
    if (s->syslog_file)
        setvbuf(s->syslog_file, NULL, _IOLBF, 0);
    else
        ND_LOGW("syslog: cannot open %s", path);
    nd_mutex_unlock(&s->syslog_lock);
}

void nd_syslog_log_close(nd_service *s)
{
    if (!s || !s->syslog_lock_init)
        return;
    nd_mutex_lock(&s->syslog_lock);
    if (s->syslog_file) {
        fflush(s->syslog_file);
        fclose(s->syslog_file);
        s->syslog_file = NULL;
    }
    s->syslog_day[0] = '\0';
    nd_mutex_unlock(&s->syslog_lock);
}

static void syslog_append(nd_service *s, const char *raw, size_t len)
{
    char day[16];
    time_t now = time(NULL);
    struct tm tm_now;

    if (!s->syslog_lock_init)
        return;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(day, sizeof(day), "%Y%m%d", &tm_now);

    nd_mutex_lock(&s->syslog_lock);
    /* 跨天：换文件 */
    if (!s->syslog_file || strcmp(s->syslog_day, day) != 0) {
        if (s->syslog_file) {
            fflush(s->syslog_file);
            fclose(s->syslog_file);
            s->syslog_file = NULL;
        }
        {
            char path[600];
            nd_strlcpy(s->syslog_day, day, sizeof(s->syslog_day));
            syslog_make_path(s, path, sizeof(path), day);
            s->syslog_file = fopen(path, "ab");
            if (s->syslog_file)
                setvbuf(s->syslog_file, NULL, _IOLBF, 0);
        }
    }
    if (s->syslog_file) {
        fwrite(raw, 1, len, s->syslog_file);
        fputc('\n', s->syslog_file);
        fflush(s->syslog_file);
    }
    nd_mutex_unlock(&s->syslog_lock);
}

/* ---------- 事件广播 + 统一入口 ---------- */

void nd_syslog_handle(nd_service *s, const nd_addr *peer, const char *raw, size_t len)
{
    nd_syslog_msg m;
    char peer_buf[96] = "";
    char text[2400];
    cJSON *o;
    char *json;

    /* 去行尾 */
    while (len && (raw[len - 1] == '\n' || raw[len - 1] == '\r'))
        len--;
    if (!len)
        return;

    if (getenv("NETDEBUG_SYSLOG_DEBUG"))
        fprintf(stderr, "[syslog-debug] len=%zu raw='%.*s'\n", len,
                (int)(len > 140 ? 140 : len), raw);

    nd_syslog_parse(raw, len, &m);
    if (peer && peer->family)
        nd_addr_format(peer, 1, peer_buf, sizeof(peer_buf));

    /* 显示文本：有解析就用结构化形式，否则原文 */
    if (m.severity >= 0) {
        const char *tag = m.tag[0] ? m.tag : nd_syslog_facility_name(m.facility);
        snprintf(text, sizeof(text), "%s%s %s%s", m.host[0] ? m.host : "-",
                 m.host[0] ? " " : "", tag, m.body[0] ? ": " : "");
        if (m.body[0]) {
            size_t used = strlen(text);
            snprintf(text + used, sizeof(text) - used, "%s", m.body);
        }
    } else {
        snprintf(text, sizeof(text), "%.*s",
                 (int)(len > sizeof(text) - 1 ? sizeof(text) - 1 : len), raw);
    }

    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "syslog_msg");
    cJSON_AddNumberToObject(o, "svc", (double)s->id);
    cJSON_AddNumberToObject(o, "sev", m.severity);
    cJSON_AddNumberToObject(o, "fac", m.facility);
    cJSON_AddStringToObject(o, "sev_name", nd_syslog_severity_name(m.severity));
    cJSON_AddStringToObject(o, "fac_name", nd_syslog_facility_name(m.facility));
    cJSON_AddStringToObject(o, "peer", peer_buf);
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddNumberToObject(o, "ts", (double)nd_wall_ms());
    json = cJSON_PrintUnformatted(o);
    if (json) {
        nd_ws_event_json(json);
        free(json);
    }
    cJSON_Delete(o);

    syslog_append(s, raw, len);
    atomic_fetch_add(&s->syslog_msgs, 1);
}

/* ---------- TCP 分帧（RFC 6587） ---------- */

/* 缓冲区里已有的内容：
 * - 以 "N " 开头且长度够 N → octet 帧（返回帧长，*need_more=0）
 * - 有 '\n' → 行帧（返回行长不含 \n，*need_more=0）
 * - 否则 *need_more=1 */
static int next_frame(const char *buf, size_t blen, int *octet)
{
    size_t i;
    if (!blen)
        return -1;
    if (buf[0] >= '0' && buf[0] <= '9') {
        size_t d = 0;
        while (d < blen && buf[d] >= '0' && buf[d] <= '9')
            d++;
        if (d && d < blen && buf[d] == ' ' && d <= 9) {
            size_t n = 0;
            for (size_t k = 0; k < d; k++)
                n = n * 10 + (size_t)(buf[k] - '0');
            if (n >= 1 && n <= SYSLOG_MAX_FRAME && blen >= d + 1 + n) {
                *octet = 1;
                return (int)n;
            }
        }
    }
    for (i = 0; i < blen; i++) {
        if (buf[i] == '\n') {
            *octet = 0;
            return (int)i;
        }
    }
    return -1;
}

void *nd_syslog_tcp_thread(void *arg)
{
    nd_syslog_tcp_job *j = (nd_syslog_tcp_job *)arg;
    char *buf = NULL;
    size_t blen = 0, bcap = 0;

    nd_net_set_timeouts(j->fd, 1000, 5000);

    while (!atomic_load(&j->conn->closing)) {
        char tmp[8192];
        int n = nd_net_recv_some(j->fd, tmp, sizeof(tmp), 1000);
        if (n > 0) {
            if (getenv("NETDEBUG_SYSLOG_DEBUG"))
                fprintf(stderr, "[syslog-debug] tcp recv n=%d blen=%zu\n", n, blen);
            if (blen + (size_t)n > SYSLOG_MAX_FRAME * 2) {
                /* 没有帧边界的垃圾流：丢到下一个换行 */
                const char *nl = (const char *)memchr(tmp, '\n', (size_t)n);
                if (nl) {
                    size_t skip = (size_t)(nl - tmp) + 1;
                    nd_conn_note_rx(j->conn, tmp, skip, NULL);
                    blen = 0;
                } else {
                    nd_conn_note_rx(j->conn, tmp, (size_t)n, NULL);
                }
                continue;
            }
            if (blen + (size_t)n > bcap) {
                size_t ncap = bcap ? bcap * 2 : 16384;
                while (ncap < blen + (size_t)n)
                    ncap *= 2;
                char *nb = (char *)realloc(buf, ncap);
                if (!nb) {
                    break;
                }
                buf = nb;
                bcap = ncap;
            }
            memcpy(buf + blen, tmp, (size_t)n);
            blen += (size_t)n;
        } else if (n == -2) {
            /* 超时：也检查是否有行帧可发 */
        } else if (n == 0) {
            break; /* 对端关闭 */
        } else {
            break;
        }

        /* 尽量把已有的完整帧都处理掉。
         * 注意：先处理再 memmove——memmove 会覆盖 frame 指向的内容 */
        for (;;) {
            int octet = 0;
            int flen = next_frame(buf ? buf : "", blen, &octet);
            if (flen < 0)
                break;
            const char *frame;
            size_t frame_len;
            size_t consume;
            if (octet) {
                size_t d = 0;
                while (d < blen && buf[d] >= '0' && buf[d] <= '9')
                    d++;
                frame = buf + d + 1;
                frame_len = (size_t)flen;
                consume = d + 1 + frame_len;
            } else {
                frame = buf;
                frame_len = (size_t)flen;
                consume = (size_t)flen + 1; /* 含 \n */
            }
            nd_conn_note_rx(j->conn, frame, frame_len, NULL);
            nd_syslog_handle(j->svc, &j->peer, frame, frame_len);
            memmove(buf, buf + consume, blen - consume);
            blen -= consume;
        }
    }

    free(buf);
    nd_conn_close(j->conn);
    nd_conn_put(j->conn);
    nd_service_put(j->svc);
    free(j);
    return NULL;
}
