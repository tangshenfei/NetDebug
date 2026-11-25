#ifndef ND_NETDIAG_H
#define ND_NETDIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 网络诊断：ping / ARP 表 / 端口扫描 / DNS 查询 */

typedef struct nd_ping_reply {
    int      seq;         /* 序号，从 1 开始 */
    int      ok;          /* 1 = 收到回应 */
    int      ttl;         /* 回复 TTL，0 = 未知 */
    int      bytes;       /* 回复负载字节数 */
    uint64_t rtt_us;      /* 往返微秒；失败时为 0 */
    char     addr[64];    /* 回复源地址（可能不同于目标） */
    char     err[96];     /* 失败原因 */
} nd_ping_reply;

/* 对 host 连续发 count 个 ICMP Echo，逐包写入 out（最多 cap 条）。
 * timeout_ms 是单包超时，间隔 1000ms。
 * 返回实际写入的条数，<0 = 参数或初始化错误。 */
int nd_net_ping(const char *host, int count, int size, int timeout_ms,
                nd_ping_reply *out, int cap);

typedef struct nd_arp_entry {
    char ip[64];
    char mac[32];
    char iface[64];
    int  state;      /* 1 = 可达/有效，0 = 其他 */
} nd_arp_entry;

/* 读取本机 ARP 邻居表。返回条数，<0 = 错误。 */
int nd_net_arp_table(nd_arp_entry *out, int cap);

typedef struct nd_scan_result {
    int  port;
    int  open;            /* 1 = 端口开放 */
    char banner[128];     /* 连接后立刻读到的服务横幅（可能为空） */
} nd_scan_result;

/* TCP connect 扫描：ports 为逗号或短横线分隔的端口列表/区间。
 * 返回扫描条数，<0 = 参数错误。 */
int nd_net_port_scan(const char *host, const char *ports, int timeout_ms,
                     nd_scan_result *out, int cap);

typedef struct nd_dns_ans {
    char type[12];   /* A / AAAA / CNAME / PTR / MX / NS / TXT */
    char value[256];
    int  ttl;
} nd_dns_ans;

/* DNS 查询。type 支持 "A"/"AAAA"/"CNAME"/"MX"/"NS"/"TXT"/"PTR"。
 * server 为空时用系统默认 DNS。返回答案条数，<0 = 错误（err 有说明）。 */
int nd_net_dns_query(const char *name, const char *type, const char *server,
                     nd_dns_ans *out, int cap, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* ND_NETDIAG_H */
