#ifndef ND_SERVICE_H
#define ND_SERVICE_H

#include "core/thread.h"
#include "net/planner.h"
#include "session/conn.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* syslog 最近消息环形缓冲：服务详情页打开时回放历史用 */
#define ND_SYSLOG_RING 500
#define ND_SYSLOG_TEXT 1024

typedef struct nd_syslog_rec {
    uint64_t ts;
    int      sev;
    int      fac;
    char     host[64];   /* 设备主机名（可能为空） */
    char     tag[48];    /* tag / app-name */
    char     peer[64];   /* 来源地址 */
    char     text[ND_SYSLOG_TEXT];  /* "host tag: body" 完整可读文本 */
    char     body[ND_SYSLOG_TEXT];  /* 仅消息体，详情页分列显示用 */
} nd_syslog_rec;

/* HTTP 请求 / FTP·TFTP 传输记录环形缓冲：服务详情页回放用 */
#define ND_HTTP_RING 500

typedef struct nd_http_rec {
    uint64_t    ts;
    int         status;     /* HTTP 状态码；FTP/TFTP 传输为 0 */
    int         dur_ms;
    long long   bytes;      /* FTP/TFTP 传输字节数 */
    char        kind[12];   /* "http" / "ftp-up" / "ftp-down" / "tftp-up" / "tftp-down" */
    char        method[12]; /* HTTP 方法；FTP/TFTP 为 RETR/STOR/RRQ/WRQ */
    char        path[256];  /* HTTP 路径；FTP/TFTP 为文件名 */
    char        peer[64];
} nd_http_rec;

typedef struct nd_service {
    uint64_t      id;
    char          name[64];
    char          proto[16];
    nd_bind_spec  bind;
    nd_plan       plan;

    /* syslog 服务：UDP 主绑定 + TCP 副绑定（RFC 6587） */
    nd_bind_spec  bind2;
    nd_plan       plan2;
    int           has_bind2;
    nd_mutex      syslog_lock;
    int           syslog_lock_init;
    FILE         *syslog_file;
    char          syslog_day[16];
    atomic_uint_fast64_t syslog_msgs;
    nd_syslog_rec syslog_ring[ND_SYSLOG_RING];
    size_t        syslog_head;   /* 下一个写入位置 */
    size_t        syslog_count;  /* 有效条数 */
    nd_http_rec   http_ring[ND_HTTP_RING];
    size_t        http_head;
    size_t        http_count;

    nd_thread     th_accept;
    atomic_int    refs;
    atomic_bool   running;
    atomic_bool   stopping;

    nd_mutex      lock;
    nd_conn     **conns;
    size_t        n_conns;
    size_t        cap_conns;
    size_t        max_conns;
    int           echo;

    /* FTP 服务端配置 */
    char          ftp_root[512];
    int           ftp_readonly;
    int           ftp_anonymous;
    char          ftp_user[64];
    char          ftp_pass[64];
    uint16_t      pasv_min;
    uint16_t      pasv_max;
    char          ftp_advertise[64];

    /* TFTP 服务端配置 */
    char          tftp_root[512];
    int           tftp_readonly;
    int           tftp_timeout;

    /* HTTP 服务端：静态文件根目录；echo=1 时返回请求内容。 */
    char          http_root[512];
    atomic_int    http_active;
    atomic_uint_fast64_t http_requests;

    /* 指标：本次启动时刻（单调时钟，0=未运行）与峰值并发 */
    atomic_uint_fast64_t started_ms;
    atomic_int    peak_conns;

    char          last_error[256];
    struct nd_service *next;
} nd_service;

nd_service *nd_service_new(const char *name, const char *proto,
                           const nd_bind_spec *bind, int echo);
int   nd_service_start(nd_service *s, char *err, size_t errlen);
void  nd_service_stop(nd_service *s);
void  nd_service_free(nd_service *s); /* 停止并从注册表移除 */

nd_service *nd_service_get(uint64_t id);
void        nd_service_put(nd_service *s);

size_t nd_service_list(uint64_t *ids, size_t cap);
const char *nd_service_proto(const nd_service *s);
int   nd_service_running(const nd_service *s);
size_t nd_service_conn_count(nd_service *s);

void  nd_service_detach_conn(uint64_t service_id, nd_conn *c);

/* 取最近 syslog 消息（按时间顺序拷到 out，返回条数）。
 * skip_last=1 时跳过最后一条（调用方已有该条，避免重复）。 */
size_t nd_service_syslog_snapshot(nd_service *s, nd_syslog_rec *out,
                                  size_t cap, int skip_last);
void   nd_service_syslog_clear(nd_service *s);

/* HTTP 请求记录：快照（时间顺序）与清空 */
size_t nd_service_http_snapshot(nd_service *s, nd_http_rec *out, size_t cap);
void   nd_service_http_clear(nd_service *s);
/* 追加一条 HTTP 请求记录（http_server.c 调用；内部加锁） */
void   nd_service_http_push(nd_service *s, const nd_http_rec *r);

#endif /* ND_SERVICE_H */
