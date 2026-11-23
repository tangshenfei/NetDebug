#ifndef ND_SYSLOG_SERVER_H
#define ND_SYSLOG_SERVER_H

#include "net/planner.h"
#include "session/service.h"

#include <stddef.h>

/* syslog 服务器：
 * - UDP 主绑定（RFC 3164 / RFC 5424 都收）
 * - TCP 副绑定（RFC 6587：octet-counting 与换行分帧都支持）
 * 每条消息：解析 PRI/级别 → 广播事件（服务实时日志按级别着色）→ 落盘
 * logs/syslog-<svc>-<YYYYMMDD>.log（跨天自动切换） */

typedef struct nd_syslog_msg {
    int    facility;   /* 0..23，-1 = 未知 */
    int    severity;   /* 0..7，-1 = 未知 */
    char   pri[12];    /* 原始 "<n>" */
    char   version[8]; /* "1"（RFC5424）或空 */
    char   ts[64];     /* 时间戳原文 */
    char   host[128];  /* 主机名 */
    char   tag[96];    /* tag 或 app-name */
    char   body[2048]; /* 消息正文 */
} nd_syslog_msg;

/* 解析一条消息（不含换行）。返回 0 = 成功，-1 = 不是合法 syslog（m 仍填默认值） */
int  nd_syslog_parse(const char *raw, size_t len, nd_syslog_msg *m);
const char *nd_syslog_severity_name(int sev);
const char *nd_syslog_facility_name(int fac);

/* 服务层处理入口：解析 → 事件广播 → 落盘 + 计数 */
void nd_syslog_handle(nd_service *s, const nd_addr *peer, const char *raw, size_t len);

/* 落盘生命周期（start/stop 时调用；跨天自动重开在 handle 内完成） */
void nd_syslog_log_open(nd_service *s);
void nd_syslog_log_close(nd_service *s);

/* TCP 连接分帧读线程：参数为 nd_syslog_tcp_job*，线程自持引用 */
typedef struct nd_syslog_tcp_job {
    nd_service *svc;
    nd_conn    *conn;
    int         fd;
    nd_addr     peer;
} nd_syslog_tcp_job;

void *nd_syslog_tcp_thread(void *arg);

#endif /* ND_SYSLOG_SERVER_H */
