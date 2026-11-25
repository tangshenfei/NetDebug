#include "platform/netdiag.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "net/addr.h"
#include "platform/netio.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* ---------- 通用：解析主机到 IPv4 ---------- */

static int resolve_v4(const char *host, struct in_addr *out, char *err, size_t errlen)
{
    struct addrinfo hints, *res = NULL;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        if (err)
            snprintf(err, errlen, "无法解析 %s", host);
        return -1;
    }
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* ---------- Ping ---------- */

#ifdef _WIN32

/* 解析 ping.exe 的一行输出，成功返回 1 并填好 r */
static int parse_ping_line(const char *line, nd_ping_reply *r)
{
    const char *ttl_p = strstr(line, "TTL=");
    const char *tim_p = strstr(line, "time");
    const char *tim_cn = strstr(line, "时间");
    const char *byt_p = strstr(line, "bytes");
    const char *byt_cn = strstr(line, "字节");
    const char *eq;
    double ms = -1;
    int bytes = 0;

    if (!ttl_p)
        return 0;
    memset(r, 0, sizeof(*r));
    r->ok = 1;
    r->ttl = atoi(ttl_p + 4);

    if (byt_p) {
        const char *b = strchr(byt_p, '=');
        if (b)
            bytes = atoi(b + 1);
    } else if (byt_cn) {
        const char *b = strchr(byt_cn, '=');
        if (b)
            bytes = atoi(b + 1);
    }
    r->bytes = bytes;

    eq = tim_cn ? strchr(tim_cn, '=') : (tim_p ? strchr(tim_p, '=') : NULL);
    if (eq) {
        if (strchr(eq, '<'))
            ms = 0.5; /* 时间<1ms */
        else if (eq[1] == '=' || eq[1] == '<')
            ms = atof(eq + 2);
        else if (eq[2] == '=' || eq[2] == '>')
            ms = atof(eq + 3);
        else
            ms = atof(eq + 1);
    }
    r->rtt_us = ms > 0 ? (uint64_t)(ms * 1000.0) : 0;

    {
        const char *from_cn = strstr(line, "来自");
        const char *from_en = strstr(line, "from");
        const char *src = from_cn ? from_cn + 6 : (from_en ? from_en + 4 : NULL);
        if (src) {
            const char *p = src;
            size_t k = 0;
            while (*p == ' ' || *p == '\t')
                p++;
            while (p[k] && p[k] != ' ' && p[k] != ':' && p[k] != '\r' && p[k] != '\n' &&
                   k + 1 < sizeof(r->addr))
                k++;
            memcpy(r->addr, p, k);
            r->addr[k] = '\0';
        }
    }
    return 1;
}

/* 非管理员权限下 IcmpSendEcho 可能直接失败（实测报超时/拒绝访问）。
 * 回退到系统 ping.exe，输出经 cmd 重定向到临时文件再读回：
 * 不用管道、不继承任何句柄（bInheritHandles=FALSE），
 * 避免 NetDebug 自己的套接字句柄被一起继承。 */
static int ping_via_exe(const char *host, int count, int size, int timeout_ms,
                        nd_ping_reply *out, int cap)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[900];
    char tmpfile[MAX_PATH];
    static char buf[8192];
    size_t total = 0;
    int n = 0;
    FILE *fp;
    BOOL ok;

    if (!GetTempFileNameA(nd_config_dir()[0] ? nd_config_dir() : ".", "png", 0, tmpfile)) {
        char statedir[MAX_PATH];
        DWORD g = GetTempPathA(sizeof(statedir), statedir);
        if (g == 0 || g >= sizeof(statedir))
            return -1;
        if (!GetTempFileNameA(statedir, "png", 0, tmpfile))
            return -1;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    snprintf(cmdline, sizeof(cmdline),
             "cmd.exe /c ping.exe -n %d -w %d -l %d %s > \"%s\" 2>&1",
             count, timeout_ms, size, host, tmpfile);

    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        DeleteFileA(tmpfile);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    fp = fopen(tmpfile, "rb");
    if (fp) {
        total = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
    }
    DeleteFileA(tmpfile);
    buf[total] = '\0';
    ND_LOGI("ping.exe 回退: -n %d -w %d -l %d %s，读到 %u 字节", count, timeout_ms, size,
            host, (unsigned)total);
    {
        /* 把原始输出压成一行写进日志，方便核对解析失败的原因 */
        char flat[512];
        size_t o = 0;
        for (size_t k = 0; k < total && o + 1 < sizeof(flat); k++) {
            char c = buf[k];
            if (c == '\r' || c == '\n' || c == '\t')
                c = '|';
            if ((unsigned char)c < 0x20)
                c = '.';
            flat[o++] = c;
        }
        flat[o] = '\0';
        ND_LOGI("ping.exe 原始输出: %s", flat);
    }

    {
        char *line = buf;
        while (line && *line && n < cap) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            if (parse_ping_line(line, &out[n])) {
                out[n].seq = n + 1;
                n++;
            }
            line = nl ? nl + 1 : NULL;
        }
    }

    if (n == 0) {
        /* ping.exe 也失败：ICMP 被系统拒绝（非管理员 + 安全策略）时它报「一般故障」 */
        int denied = (strstr(buf, "一般故障") != NULL ||
                      strstr(buf, "General failure") != NULL ||
                      strstr(buf, "transmit failed") != NULL);
        const char *why = denied
                              ? "ICMP 被系统拒绝，请以管理员身份运行 NetDebug 再试"
                              : "请求超时";
        for (int i = 0; i < count && i < cap; i++) {
            memset(&out[i], 0, sizeof(out[i]));
            out[i].seq = i + 1;
            snprintf(out[i].err, sizeof(out[i].err), "%s", why);
            n++;
        }
    }
    return n;
}

int nd_net_ping(const char *host, int count, int size, int timeout_ms,
                nd_ping_reply *out, int cap)
{
    struct in_addr addr;
    char errbuf[128] = "";
    HANDLE h;
    char *payload;
    int i, n = 0;
    int ok_count = 0;
    DWORD last_err = 0;

    if (!host || !host[0] || count <= 0 || !out || cap <= 0)
        return -1;
    if (size < 8)
        size = 32;
    if (size > 1400)
        size = 1400;
    if (count > cap)
        count = cap;
    if (resolve_v4(host, &addr, errbuf, sizeof(errbuf)) != 0) {
        for (i = 0; i < count; i++) {
            memset(&out[i], 0, sizeof(out[i]));
            out[i].seq = i + 1;
            snprintf(out[i].err, sizeof(out[i].err), "%s", errbuf);
        }
        return count;
    }

    h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(errbuf, sizeof(errbuf), "创建 ICMP 句柄失败（可能需要管理员权限）");
        for (i = 0; i < count; i++) {
            memset(&out[i], 0, sizeof(out[i]));
            out[i].seq = i + 1;
            snprintf(out[i].err, sizeof(out[i].err), "%s", errbuf);
        }
        return count;
    }

    payload = (char *)calloc(1, (size_t)size);
    if (!payload) {
        IcmpCloseHandle(h);
        return -1;
    }
    for (i = 0; i < size; i++)
        payload[i] = (char)('a' + (i % 26));

    for (i = 0; i < count; i++) {
        nd_ping_reply *r = &out[i];
        unsigned char reply[sizeof(ICMP_ECHO_REPLY) + 1500 + 16];
        DWORD rc;
        uint64_t t0;

        memset(r, 0, sizeof(*r));
        r->seq = i + 1;
        t0 = nd_monotonic_ms();
        rc = IcmpSendEcho(h, addr.s_addr, payload, (WORD)size, NULL, reply,
                          sizeof(reply), (DWORD)timeout_ms);
        r->rtt_us = (nd_monotonic_ms() - t0) * 1000;
        if (rc > 0) {
            PICMP_ECHO_REPLY rep = (PICMP_ECHO_REPLY)reply;
            struct in_addr from;
            char ipbuf[64];
            from.s_addr = rep->Address;
            r->ok = (rep->Status == IP_SUCCESS) ? 1 : 0;
            if (r->ok) ok_count++;
            r->ttl = (int)rep->Options.Ttl;
            r->bytes = (int)rep->DataSize;
            r->rtt_us = (uint64_t)rep->RoundTripTime * 1000;
            if (inet_ntop(AF_INET, &from, ipbuf, sizeof(ipbuf)))
                nd_strlcpy(r->addr, ipbuf, sizeof(r->addr));
            if (!r->ok)
                snprintf(r->err, sizeof(r->err), "ICMP 状态 %lu", (unsigned long)rep->Status);
        } else {
            DWORD e = GetLastError();
            last_err = e;
            if (e == IP_REQ_TIMED_OUT)
                snprintf(r->err, sizeof(r->err), "请求超时");
            else if (e == IP_GENERAL_FAILURE)
                snprintf(r->err, sizeof(r->err), "目标不可达");
            else
                snprintf(r->err, sizeof(r->err), "失败（错误码 %lu）", (unsigned long)e);
        }
        n++;
    }
    free(payload);
    IcmpCloseHandle(h);

    /* 一个回应都没收到时的处理：
     * 错误码 5（拒绝访问）= 系统/安全软件不允许非管理员进程发 ICMP，
     * 这种情况直接给明确提示，不再浪费一次 ping.exe。 */
    if (ok_count == 0) {
        if (last_err == 5 || last_err == ERROR_ACCESS_DENIED) {
            for (i = 0; i < n; i++) {
                out[i].ok = 0;
                snprintf(out[i].err, sizeof(out[i].err),
                         "ICMP 被系统拒绝，请以管理员身份运行 NetDebug");
            }
            return n;
        }
        return ping_via_exe(host, count, size, timeout_ms, out, cap);
    }
    return n;
}

#else /* POSIX：raw ICMP socket（需要 root 或 CAP_NET_RAW） */

static uint16_t icmp_csum(const void *data, size_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len)
        sum += *(const uint8_t *)p;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

int nd_net_ping(const char *host, int count, int size, int timeout_ms,
                nd_ping_reply *out, int cap)
{
    struct in_addr addr;
    char errbuf[128] = "";
    int fd, i, n = 0;
    struct sockaddr_in dst;
    uint16_t ident;

    if (!host || !host[0] || count <= 0 || !out || cap <= 0)
        return -1;
    if (size < 8)
        size = 32;
    if (size > 1400)
        size = 1400;
    if (count > cap)
        count = cap;

    if (resolve_v4(host, &addr, errbuf, sizeof(errbuf)) != 0) {
        for (i = 0; i < count; i++) {
            memset(&out[i], 0, sizeof(out[i]));
            out[i].seq = i + 1;
            snprintf(out[i].err, sizeof(out[i].err), "%s", errbuf);
        }
        return count;
    }

    fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        snprintf(errbuf, sizeof(errbuf), "无法创建 ICMP 套接字（需要 root 或 CAP_NET_RAW）");
        for (i = 0; i < count; i++) {
            memset(&out[i], 0, sizeof(out[i]));
            out[i].seq = i + 1;
            snprintf(out[i].err, sizeof(out[i].err), "%s", errbuf);
        }
        return count;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr = addr;
    ident = (uint16_t)(nd_wall_ms() & 0xffff);

    for (i = 0; i < count; i++) {
        nd_ping_reply *r = &out[i];
        unsigned char pkt[1500];
        int plen = size;
        struct icmphdr *icmp = (struct icmphdr *)pkt;
        uint64_t t0 = nd_monotonic_ms();
        int got = 0;

        memset(r, 0, sizeof(*r));
        r->seq = i + 1;

        memset(pkt, 0, sizeof(pkt));
        icmp->type = ICMP_ECHO;
        icmp->code = 0;
        icmp->un.echo.id = htons(ident);
        icmp->un.echo.sequence = htons((uint16_t)r->seq);
        for (int k = 8; k < plen; k++)
            pkt[k] = (unsigned char)('a' + (k % 26));
        icmp->checksum = icmp_csum(pkt, (size_t)plen);

        if (sendto(fd, pkt, (size_t)plen, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            snprintf(r->err, sizeof(r->err), "发送失败");
        } else {
            /* 等回复，超时则记失败 */
            uint64_t deadline = t0 + (uint64_t)timeout_ms;
            while (!got) {
                struct pollfd pfd;
                int pr;
                uint64_t now = nd_monotonic_ms();
                if (now >= deadline)
                    break;
                pfd.fd = fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                pr = poll(&pfd, 1, (int)(deadline - now));
                if (pr <= 0)
                    continue;
                {
                    unsigned char buf[1500];
                    struct sockaddr_in from;
                    socklen_t fl = sizeof(from);
                    int rn = (int)recvfrom(fd, buf, sizeof(buf), 0,
                                           (struct sockaddr *)&from, &fl);
                    if (rn < 20)
                        continue;
                    {
                        int ihl = (buf[0] & 0x0f) * 4;
                        struct icmphdr *ri;
                        if (rn < ihl + 8)
                            continue;
                        ri = (struct icmphdr *)(buf + ihl);
                        if (ri->type != ICMP_ECHOREPLY)
                            continue;
                        if (ntohs(ri->un.echo.id) != ident)
                            continue;
                        if (ntohs(ri->un.echo.sequence) != (uint16_t)r->seq)
                            continue;
                        {
                            char ipbuf[64];
                            r->ok = 1;
                            r->ttl = buf[8];
                            r->bytes = rn - ihl - 8;
                            r->rtt_us = (nd_monotonic_ms() - t0) * 1000;
                            if (inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf)))
                                nd_strlcpy(r->addr, ipbuf, sizeof(r->addr));
                            got = 1;
                        }
                    }
                }
            }
            if (!got)
                snprintf(r->err, sizeof(r->err), "请求超时");
        }
        n++;
        if (i + 1 < count) {
            int spent = (int)(nd_monotonic_ms() - t0);
            if (spent < 1000)
                nd_sleep_ms(1000 - spent);
        }
    }
    close(fd);
    return n;
}

#endif /* _WIN32 */

/* ---------- ARP 表 ---------- */

#ifdef _WIN32

int nd_net_arp_table(nd_arp_entry *out, int cap)
{
    ULONG size = 0;
    PMIB_IPNETTABLE table;
    int n = 0;

    if (!out || cap <= 0)
        return -1;
    if (GetIpNetTable(NULL, &size, 0) != ERROR_INSUFFICIENT_BUFFER)
        return -1;
    table = (PMIB_IPNETTABLE)malloc(size);
    if (!table)
        return -1;
    if (GetIpNetTable(table, &size, 0) != NO_ERROR) {
        free(table);
        return -1;
    }
    for (DWORD i = 0; i < table->dwNumEntries && n < cap; i++) {
        MIB_IPNETROW *row = &table->table[i];
        unsigned char *m = row->bPhysAddr;
        if (row->dwPhysAddrLen != 6)
            continue;
        snprintf(out[n].ip, sizeof(out[n].ip), "%u.%u.%u.%u",
                 row->dwAddr & 0xff, (row->dwAddr >> 8) & 0xff,
                 (row->dwAddr >> 16) & 0xff, (row->dwAddr >> 24) & 0xff);
        snprintf(out[n].mac, sizeof(out[n].mac), "%02X-%02X-%02X-%02X-%02X-%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        out[n].iface[0] = '\0';
        out[n].state = (row->dwType == MIB_IPNET_TYPE_DYNAMIC ||
                        row->dwType == MIB_IPNET_TYPE_STATIC)
                           ? 1
                           : 0;
        n++;
    }
    free(table);
    return n;
}

#else

int nd_net_arp_table(nd_arp_entry *out, int cap)
{
    FILE *f;
    char line[512];
    int n = 0;

    if (!out || cap <= 0)
        return -1;
    f = fopen("/proc/net/arp", "r");
    if (!f)
        return -1;
    /* 跳过表头 */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    while (n < cap && fgets(line, sizeof(line), f)) {
        char ip[64] = "", hw[8], mac[32] = "", mask[64] = "", dev[64] = "";
        unsigned flags = 0;
        if (sscanf(line, "%63s %7s %x %31s %63s %63s", ip, hw, &flags, mac, mask, dev) < 4)
            continue;
        nd_strlcpy(out[n].ip, ip, sizeof(out[n].ip));
        nd_strlcpy(out[n].mac, mac, sizeof(out[n].mac));
        nd_strlcpy(out[n].iface, dev, sizeof(out[n].iface));
        out[n].state = (flags & 0x2) ? 1 : 0;
        n++;
    }
    fclose(f);
    return n;
}

#endif

/* ---------- 端口扫描 ---------- */

static int scan_one(const char *host, int port, int timeout_ms, char *banner, size_t bcap)
{
    int fd;
    nd_addr target;
    char spec[300];
    int r;

    if (strchr(host, ':'))
        snprintf(spec, sizeof(spec), "[%s]:%d", host, port);
    else
        snprintf(spec, sizeof(spec), "%s:%d", host, port);
    if (nd_addr_parse(spec, port, &target) != 0)
        return 0;

    fd = nd_net_socket(target.family, SOCK_STREAM);
    if (fd < 0)
        return 0;
    r = nd_net_connect(fd, &target, timeout_ms, NULL);
    if (r != 0) {
        nd_net_close(fd);
        return 0;
    }
    /* 抓 banner：nd_net_recv_some 不使用它的 timeout 参数，
     * 靠 socket 级的接收超时（不设会一直阻塞到 TCP 超时，实测 30 秒）。 */
    if (banner && bcap > 1) {
        int n;
        nd_net_set_timeouts(fd, 300, 1000);
        n = nd_net_recv_some(fd, banner, (size_t)(bcap - 1), 300);
        if (n > 0) {
            size_t i;
            banner[n] = '\0';
            for (i = 0; i < (size_t)n; i++)
                if (banner[i] == '\r' || banner[i] == '\n')
                    banner[i] = ' ';
        }
    }
    nd_net_close(fd);
    return 1;
}

int nd_net_port_scan(const char *host, const char *ports, int timeout_ms,
                     nd_scan_result *out, int cap)
{
    int idx = 0;
    const char *p;

    if (!host || !ports || !out || cap <= 0)
        return -1;
    if (timeout_ms <= 0)
        timeout_ms = 800;

    p = ports;
    while (*p && idx < cap) {
        int lo = 0, hi = 0;
        while (*p == ' ' || *p == ',' || *p == ';')
            p++;
        if (!*p)
            break;
        if (*p >= '0' && *p <= '9') {
            lo = (int)strtol(p, (char **)&p, 10);
            hi = lo;
            if (*p == '-') {
                p++;
                if (*p >= '0' && *p <= '9')
                    hi = (int)strtol(p, (char **)&p, 10);
            }
        } else {
            /* 跳过无法识别的内容 */
            p++;
            continue;
        }
        if (lo < 1 || lo > 65535)
            continue;
        if (hi < lo)
            hi = lo;
        if (hi > 65535)
            hi = 65535;
        for (int port = lo; port <= hi && idx < cap; port++) {
            char banner[128] = "";
            out[idx].port = port;
            out[idx].open = scan_one(host, port, timeout_ms, banner, sizeof(banner));
            nd_strlcpy(out[idx].banner, banner, sizeof(out[idx].banner));
            idx++;
        }
    }
    return idx;
}

/* ---------- DNS 查询 ---------- */

static int dns_type_code(const char *t)
{
    if (!t || !t[0] || !strcmp(t, "A")) return 1;
    if (!strcmp(t, "NS")) return 2;
    if (!strcmp(t, "CNAME")) return 5;
    if (!strcmp(t, "PTR")) return 12;
    if (!strcmp(t, "MX")) return 15;
    if (!strcmp(t, "TXT")) return 16;
    if (!strcmp(t, "AAAA")) return 28;
    return 1;
}

static const char *dns_type_name(int code)
{
    switch (code) {
    case 1: return "A";
    case 2: return "NS";
    case 5: return "CNAME";
    case 12: return "PTR";
    case 15: return "MX";
    case 16: return "TXT";
    case 28: return "AAAA";
    default: return "?";
    }
}

static size_t dns_encode_name(const char *name, unsigned char *dst, size_t cap)
{
    size_t o = 0;
    const char *p = name;

    while (*p && o + 1 < cap) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63)
            return (size_t)-1;
        if (o + 1 + len >= cap)
            return (size_t)-1;
        dst[o++] = (unsigned char)len;
        memcpy(dst + o, p, len);
        o += len;
        if (!dot)
            break;
        p = dot + 1;
        if (!*p)
            break;
    }
    if (o + 1 >= cap)
        return (size_t)-1;
    dst[o++] = 0;
    return o;
}

/* 跳过 DNS 名称（支持压缩指针） */
static int dns_skip_name(const unsigned char *buf, size_t len, size_t *off)
{
    size_t p = *off;
    int guard = 0;

    while (p < len && guard++ < 256) {
        unsigned char c = buf[p];
        if (c == 0) {
            *off = p + 1;
            return 0;
        }
        if ((c & 0xc0) == 0xc0) {
            if (p + 1 >= len)
                return -1;
            *off = p + 2;
            return 0;
        }
        p += 1u + c;
    }
    return -1;
}

static void dns_read_name(const unsigned char *buf, size_t len, size_t off,
                          char *out, size_t cap)
{
    size_t o = 0;
    int guard = 0;

    while (off < len && guard++ < 128) {
        unsigned char c = buf[off];
        if (c == 0)
            break;
        if ((c & 0xc0) == 0xc0) {
            if (off + 1 >= len)
                break;
            off = (size_t)(((c & 0x3f) << 8) | buf[off + 1]);
            continue;
        }
        off++;
        if (o && o + 1 < cap)
            out[o++] = '.';
        for (size_t i = 0; i < c && off < len && o + 1 < cap; i++)
            out[o++] = (char)buf[off++];
    }
    out[o < cap ? o : cap - 1] = '\0';
}

#ifdef _WIN32
static int system_dns_server(char *out, size_t cap)
{
    /* 用首个启用接口的 DNS 地址 */
    ULONG size = 0;
    PIP_ADAPTER_ADDRESSES addrs;
    int found = 0;

    if (!out || cap < 16)
        return -1;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size);
    addrs = (PIP_ADAPTER_ADDRESSES)malloc(size);
    if (!addrs)
        return -1;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, addrs, &size) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a && !found; a = a->Next) {
            for (PIP_ADAPTER_DNS_SERVER_ADDRESS d = a->FirstDnsServerAddress; d && !found; d = d->Next) {
                if (d->Address.lpSockaddr &&
                    d->Address.lpSockaddr->sa_family == AF_INET) {
                    struct sockaddr_in *sin = (struct sockaddr_in *)d->Address.lpSockaddr;
                    if (inet_ntop(AF_INET, &sin->sin_addr, out, cap))
                        found = 1;
                }
            }
        }
    }
    free(addrs);
    return found ? 0 : -1;
}
#else
static int system_dns_server(char *out, size_t cap)
{
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[256];

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "nameserver", 10) == 0) {
            char ip[64] = "";
            if (sscanf(p + 10, "%63s", ip) == 1) {
                nd_strlcpy(out, ip, cap);
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return -1;
}
#endif

int nd_net_dns_query(const char *name, const char *type, const char *server,
                     nd_dns_ans *out, int cap, char *err, size_t errlen)
{
    unsigned char req[512], resp[1500];
    size_t qlen, qname_len;
    int fd, rn;
    struct sockaddr_in dst;
    uint16_t qtype = (uint16_t)dns_type_code(type);
    uint16_t id;
    char server_ip[64] = "";
    int ans_count = 0;
    size_t off;

    if (!name || !name[0] || !out || cap <= 0) {
        if (err) snprintf(err, errlen, "参数为空");
        return -1;
    }
    if (!server || !server[0]) {
        if (system_dns_server(server_ip, sizeof(server_ip)) != 0) {
            if (err) snprintf(err, errlen, "无法获取系统 DNS 服务器，请手动填写");
            return -1;
        }
        server = server_ip;
    }

    memset(req, 0, sizeof(req));
    id = (uint16_t)(nd_wall_ms() & 0xffff);
    req[0] = (unsigned char)(id >> 8);
    req[1] = (unsigned char)(id & 0xff);
    req[2] = 0x01; /* RD */
    req[3] = 0x00;
    req[4] = 0x00; req[5] = 0x01; /* QDCOUNT=1 */
    req[6] = 0; req[7] = 0; req[8] = 0; req[9] = 0; req[10] = 0; req[11] = 0;

    qname_len = dns_encode_name(name, req + 12, sizeof(req) - 16);
    if (qname_len == (size_t)-1) {
        if (err) snprintf(err, errlen, "域名格式不合法");
        return -1;
    }
    qlen = 12 + qname_len + 4;
    req[12 + qname_len + 0] = (unsigned char)(qtype >> 8);
    req[12 + qname_len + 1] = (unsigned char)(qtype & 0xff);
    req[12 + qname_len + 2] = 0;
    req[12 + qname_len + 3] = 1; /* IN */

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    if (inet_pton(AF_INET, server, &dst.sin_addr) != 1) {
        struct in_addr ia;
        if (resolve_v4(server, &ia, NULL, 0) != 0) {
            if (err) snprintf(err, errlen, "DNS 服务器地址不合法");
            return -1;
        }
        dst.sin_addr = ia;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (err) snprintf(err, errlen, "创建套接字失败");
        return -1;
    }
    {
        /* Windows 的 SO_RCVTIMEO 参数是 DWORD 毫秒，POSIX 是 struct timeval，
         * 传错类型会让超时不生效（实测表现为随机丢应答）。 */
#ifdef _WIN32
        DWORD tv = 3000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif
    }
    if (sendto(fd, (const char *)req, qlen, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        nd_net_close(fd);
        if (err) snprintf(err, errlen, "发送查询失败");
        return -1;
    }
    rn = (int)recv(fd, (char *)resp, sizeof(resp), 0);
    nd_net_close(fd);
    if (rn < 12) {
        if (err) snprintf(err, errlen, "等待 DNS 响应超时");
        return -1;
    }
    if (resp[0] != req[0] || resp[1] != req[1]) {
        if (err) snprintf(err, errlen, "响应 ID 不匹配");
        return -1;
    }
    if (resp[3] & 0x0f) {
        int rc = resp[3] & 0x0f;
        if (err) {
            const char *rs = rc == 3 ? " NXDOMAIN（域名不存在）" :
                             rc == 2 ? "SERVFAIL" : rc == 5 ? "REFUSED" : "";
            snprintf(err, errlen, "DNS 返回错误码 %d%s", rc, rs);
        }
        return -1;
    }

    {
        int qd = (resp[4] << 8) | resp[5];
        int an = (resp[6] << 8) | resp[7];
        int i;

        off = 12;
        for (i = 0; i < qd; i++) {
            if (dns_skip_name(resp, (size_t)rn, &off) != 0)
                return -1;
            off += 4;
        }
        for (i = 0; i < an && ans_count < cap; i++) {
            int rtype, rdlen;
            unsigned int ttl;
            if (dns_skip_name(resp, (size_t)rn, &off) != 0)
                break;
            if (off + 10 > (size_t)rn)
                break;
            rtype = (resp[off] << 8) | resp[off + 1];
            ttl = ((unsigned int)resp[off + 4] << 24) | ((unsigned int)resp[off + 5] << 16) |
                  ((unsigned int)resp[off + 6] << 8) | resp[off + 7];
            rdlen = (resp[off + 8] << 8) | resp[off + 9];
            off += 10;
            if (off + (size_t)rdlen > (size_t)rn)
                break;

            nd_strlcpy(out[ans_count].type, dns_type_name(rtype), sizeof(out[ans_count].type));
            out[ans_count].ttl = (int)ttl;
            out[ans_count].value[0] = '\0';

            if (rtype == 1 && rdlen == 4) {
                char ip[64];
                snprintf(ip, sizeof(ip), "%u.%u.%u.%u", resp[off], resp[off + 1],
                         resp[off + 2], resp[off + 3]);
                nd_strlcpy(out[ans_count].value, ip, sizeof(out[ans_count].value));
            } else if (rtype == 28 && rdlen == 16) {
                char ip[64];
                if (inet_ntop(AF_INET6, resp + off, ip, sizeof(ip)))
                    nd_strlcpy(out[ans_count].value, ip, sizeof(out[ans_count].value));
            } else if (rtype == 5 || rtype == 2 || rtype == 12) {
                char nm[256];
                dns_read_name(resp, (size_t)rn, off, nm, sizeof(nm));
                nd_strlcpy(out[ans_count].value, nm, sizeof(out[ans_count].value));
            } else if (rtype == 15 && rdlen >= 3) {
                char nm[256];
                int pref = (resp[off] << 8) | resp[off + 1];
                dns_read_name(resp, (size_t)rn, off + 2, nm, sizeof(nm));
                snprintf(out[ans_count].value, sizeof(out[ans_count].value), "%d %s", pref, nm);
            } else if (rtype == 16) {
                size_t o = 0, k = off;
                size_t end = off + (size_t)rdlen;
                while (k < end && o + 1 < sizeof(out[ans_count].value)) {
                    unsigned char sl = resp[k++];
                    for (unsigned char j = 0; j < sl && k < end && o + 1 < sizeof(out[ans_count].value); j++)
                        out[ans_count].value[o++] = (char)resp[k++];
                }
                out[ans_count].value[o] = '\0';
            } else {
                snprintf(out[ans_count].value, sizeof(out[ans_count].value),
                         "（%d 字节，未解析）", rdlen);
            }
            ans_count++;
            off += (size_t)rdlen;
        }
    }
    return ans_count;
}
