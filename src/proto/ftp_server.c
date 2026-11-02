#include "proto/ftp_server.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "platform/netio.h"
#include "platform/platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    int      ctrl;
    nd_addr  peer;
    nd_addr  local;
    nd_ftp_srv_cfg cfg;
    char     cwd[1024];
    int      authed;
    int      pasv_fd;
    nd_addr  pasv_local;
    nd_addr  active;
    int      ascii;
    char     rnfr[1024];
} ftp_sess;

typedef struct {
    char      name[512];
    long long size;
    time_t    mtime;
    int       is_dir;
} ftp_ent;

static void reply(int fd, int code, const char *fmt, ...)
{
    char buf[2048];
    char msg[1600];
    va_list ap;
    int n;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    n = snprintf(buf, sizeof(buf), "%d %s\r\n", code, msg);
    nd_net_send_all(fd, buf, (size_t)n, 10000);
}

static int read_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        char c;
        int r = nd_net_recv_some(fd, &c, 1, 30000);
        if (r == -2)
            return -1;
        if (r <= 0)
            return -1;
        if (c == '\n') {
            if (n && buf[n - 1] == '\r')
                n--;
            buf[n] = '\0';
            return 0;
        }
        if (n + 1 < cap)
            buf[n++] = c;
        if (n > 4000)
            return -1;
    }
}

static void virt_norm(const char *cwd, const char *arg, char *out, size_t cap)
{
    char tmp[2048];
    char segs[64][256];
    int nseg = 0;
    const char *p;
    char *save = NULL;
    char *tok;

    if (arg && arg[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", arg);
    else if (arg && arg[0])
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd[0] ? cwd : "/", arg);
    else
        snprintf(tmp, sizeof(tmp), "%s", cwd[0] ? cwd : "/");

    tok = strtok_r(tmp, "/", &save);
    while (tok && nseg < 64) {
        if (!strcmp(tok, ".") || !tok[0]) {
            /* skip */
        } else if (!strcmp(tok, "..")) {
            if (nseg > 0)
                nseg--;
        } else {
            nd_strlcpy(segs[nseg++], tok, sizeof(segs[0]));
        }
        tok = strtok_r(NULL, "/", &save);
    }
    out[0] = '\0';
    for (int i = 0; i < nseg; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "/%s", segs[i]);
    }
    if (!out[0])
        nd_strlcpy(out, "/", cap);
}

static void real_path(ftp_sess *s, const char *virt, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", s->cfg.root, virt);
}

static int file_stat(const char *path, long long *size, time_t *mtime, int *is_dir)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d))
        return -1;
    *is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    *size = ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (mtime) {
        ULARGE_INTEGER li;
        li.LowPart = d.ftLastWriteTime.dwLowDateTime;
        li.HighPart = d.ftLastWriteTime.dwHighDateTime;
        *mtime = (time_t)((li.QuadPart - 116444736000000000ULL) / 10000000ULL);
    }
    return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    *size = (long long)st.st_size;
    if (mtime)
        *mtime = st.st_mtime;
    return 0;
#endif
}

static int collect_dir(const char *path, ftp_ent **out, int *n)
{
    ftp_ent *arr = NULL;
    int cnt = 0, cap = 0;
#ifdef _WIN32
    char pattern[1200];
    WIN32_FIND_DATAA d;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    h = FindFirstFileA(pattern, &d);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, ".."))
            continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 32;
            ftp_ent *na = (ftp_ent *)realloc(arr, (size_t)cap * sizeof(*na));
            if (!na) { free(arr); FindClose(h); return -1; }
            arr = na;
        }
        memset(&arr[cnt], 0, sizeof(arr[cnt]));
        nd_strlcpy(arr[cnt].name, d.cFileName, sizeof(arr[cnt].name));
        arr[cnt].is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        arr[cnt].size = ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        {
            ULARGE_INTEGER li;
            li.LowPart = d.ftLastWriteTime.dwLowDateTime;
            li.HighPart = d.ftLastWriteTime.dwHighDateTime;
            arr[cnt].mtime = (time_t)((li.QuadPart - 116444736000000000ULL) / 10000000ULL);
        }
        cnt++;
    } while (FindNextFileA(h, &d));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    struct dirent *de;
    if (!dir)
        return -1;
    while ((de = readdir(dir)) != NULL) {
        char full[1200];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        if (stat(full, &st) != 0)
            continue;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 32;
            ftp_ent *na = (ftp_ent *)realloc(arr, (size_t)cap * sizeof(*na));
            if (!na) { free(arr); closedir(dir); return -1; }
            arr = na;
        }
        memset(&arr[cnt], 0, sizeof(arr[cnt]));
        nd_strlcpy(arr[cnt].name, de->d_name, sizeof(arr[cnt].name));
        arr[cnt].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        arr[cnt].size = (long long)st.st_size;
        arr[cnt].mtime = st.st_mtime;
        cnt++;
    }
    closedir(dir);
#endif
    *out = arr;
    *n = cnt;
    return 0;
}

static void fmt_modify(time_t t, char *out, size_t cap)
{
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%Y%m%d%H%M%S", &tmv);
}

static void fmt_list_time(time_t t, char *out, size_t cap)
{
    struct tm tmv;
    char buf[64];
    time_t now = time(NULL);
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    if (now - t > 180 * 24 * 3600)
        strftime(buf, sizeof(buf), "%b %d  %Y", &tmv);
    else
        strftime(buf, sizeof(buf), "%b %d %H:%M", &tmv);
    nd_strlcpy(out, buf, cap);
}

static int wait_data(ftp_sess *s, char *err, size_t errlen)
{
    if (s->active.family) {
        int fd = nd_net_socket(s->active.family, SOCK_STREAM);
        int se = 0;
        if (fd < 0 || nd_net_connect(fd, &s->active, 8000, &se) != 0) {
            if (fd >= 0)
                nd_net_close(fd);
            snprintf(err, errlen, "主动模式连接失败");
            return -1;
        }
        return fd;
    }
    if (s->pasv_fd >= 0) {
        nd_pollfd p;
        int rc;
        p.fd = s->pasv_fd;
        p.events = ND_POLLIN;
        p.revents = 0;
        rc = nd_net_poll(&p, 1, 10000);
        if (rc <= 0) {
            snprintf(err, errlen, "等待数据连接超时");
            return -1;
        }
        {
            nd_addr peer, local;
            int fd = nd_net_accept(s->pasv_fd, &peer, &local);
            if (fd < 0) {
                snprintf(err, errlen, "接受数据连接失败");
                return -1;
            }
            return fd;
        }
    }
    snprintf(err, errlen, "没有数据连接");
    return -1;
}

static void close_passive(ftp_sess *s)
{
    if (s->pasv_fd >= 0) {
        nd_net_close(s->pasv_fd);
        s->pasv_fd = -1;
    }
    memset(&s->active, 0, sizeof(s->active));
}

static int open_passive(ftp_sess *s, int epsv, char *err, size_t errlen)
{
    int fd;
    nd_addr addr = s->local;
    uint16_t port;

    close_passive(s);
    addr.port = 0;
    fd = nd_net_socket(addr.family, SOCK_STREAM);
    if (fd < 0) {
        snprintf(err, errlen, "创建监听失败");
        return -1;
    }
    nd_net_set_reuse(fd);
    if (s->local.family == AF_INET6)
        nd_net_set_v6only(fd, 1);
    {
        int bound = -1;
        uint16_t lo = s->cfg.pasv_min ? s->cfg.pasv_min : 50000;
        uint16_t hi = s->cfg.pasv_max ? s->cfg.pasv_max : 50100;
        for (port = lo; port <= hi; port++) {
            addr.port = port;
            if (nd_net_bind(fd, &addr) == 0) {
                bound = port;
                break;
            }
        }
        if (bound < 0 && lo == 0) {
            addr.port = 0;
            if (nd_net_bind(fd, &addr) == 0)
                bound = 0;
        }
        if (bound < 0) {
            nd_net_close(fd);
            snprintf(err, errlen, "被动端口段已耗尽");
            return -1;
        }
    }
    nd_net_listen(fd, 4);
    s->pasv_fd = fd;
    nd_net_getsockname(fd, &s->pasv_local);
    (void)epsv;
    return 0;
}

static void send_list(ftp_sess *s, int fd, const char *virt, int mlsd, int nlst)
{
    char real[1200];
    ftp_ent *ents = NULL;
    int n = 0;

    real_path(s, virt, real, sizeof(real));
    if (collect_dir(real, &ents, &n) != 0) {
        free(ents);
        reply(s->ctrl, 550, "无法列出目录");
        nd_net_close(fd);
        return;
    }
    reply(s->ctrl, 150, "Opening data connection");
    for (int i = 0; i < n; i++) {
        char line[2048];
        if (nlst) {
            snprintf(line, sizeof(line), "%s\r\n", ents[i].name);
        } else if (mlsd) {
            char mt[32];
            fmt_modify(ents[i].mtime, mt, sizeof(mt));
            snprintf(line, sizeof(line), "type=%s;size=%lld;modify=%s; %s\r\n",
                     ents[i].is_dir ? "dir" : "file", ents[i].size, mt, ents[i].name);
        } else {
            char tm[64];
            fmt_list_time(ents[i].mtime, tm, sizeof(tm));
            snprintf(line, sizeof(line), "%s 1 ftp ftp %8lld %s %s\r\n",
                     ents[i].is_dir ? "drwxr-xr-x" : "-rw-r--r--",
                     ents[i].size, tm, ents[i].name);
        }
        if (nd_net_send_all(fd, line, strlen(line), 15000) != 0)
            break;
    }
    free(ents);
    nd_net_close(fd);
    reply(s->ctrl, 226, "Transfer complete");
}

static void do_retr(ftp_sess *s, const char *virt)
{
    char real[1200];
    char err[128];
    FILE *fp;
    int dfd;

    real_path(s, virt, real, sizeof(real));
    fp = fopen(real, "rb");
    if (!fp) {
        reply(s->ctrl, 550, "文件不存在或不可读");
        return;
    }
    dfd = wait_data(s, err, sizeof(err));
    if (dfd < 0) {
        fclose(fp);
        reply(s->ctrl, 425, "%s", err);
        return;
    }
    reply(s->ctrl, 150, "Opening binary mode data connection");
    for (;;) {
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0)
            break;
        if (nd_net_send_all(dfd, buf, n, 15000) != 0)
            break;
    }
    fclose(fp);
    nd_net_close(dfd);
    close_passive(s);
    reply(s->ctrl, 226, "Transfer complete");
}

static void do_stor(ftp_sess *s, const char *virt, int append)
{
    char real[1200];
    char err[128];
    FILE *fp;
    int dfd;

    if (s->cfg.readonly) {
        reply(s->ctrl, 550, "只读模式，禁止写入");
        return;
    }
    real_path(s, virt, real, sizeof(real));
    fp = fopen(real, append ? "ab" : "wb");
    if (!fp) {
        reply(s->ctrl, 550, "无法写入目标文件");
        return;
    }
    dfd = wait_data(s, err, sizeof(err));
    if (dfd < 0) {
        fclose(fp);
        reply(s->ctrl, 425, "%s", err);
        return;
    }
    reply(s->ctrl, 150, "Ok to send data");
    for (;;) {
        char buf[65536];
        int r = nd_net_recv_some(dfd, buf, sizeof(buf), 15000);
        if (r > 0) {
            fwrite(buf, 1, (size_t)r, fp);
            continue;
        }
        if (r == -2)
            break;
        break;
    }
    fclose(fp);
    nd_net_close(dfd);
    close_passive(s);
    reply(s->ctrl, 226, "Transfer complete");
}

static int need_auth(ftp_sess *s)
{
    if (!s->authed) {
        reply(s->ctrl, 530, "Please login with USER and PASS");
        return 1;
    }
    return 0;
}

static void handle_port(ftp_sess *s, const char *arg)
{
    int h[6];
    if (sscanf(arg, "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) != 6) {
        reply(s->ctrl, 501, "Bad PORT");
        return;
    }
    if (s->peer.family != AF_INET) {
        reply(s->ctrl, 522, "Use EPRT for IPv6");
        return;
    }
    {
        nd_addr a;
        memset(&a, 0, sizeof(a));
        a.family = AF_INET;
        a.raw[0] = (uint8_t)h[0];
        a.raw[1] = (uint8_t)h[1];
        a.raw[2] = (uint8_t)h[2];
        a.raw[3] = (uint8_t)h[3];
        a.port = (uint16_t)(h[4] * 256 + h[5]);
        if (memcmp(a.raw, s->peer.raw, 4) != 0) {
            reply(s->ctrl, 501, "PORT address must match control peer");
            return;
        }
        close_passive(s);
        s->active = a;
        reply(s->ctrl, 200, "PORT command successful");
    }
}

static void handle_eprt(ftp_sess *s, const char *arg)
{
    char delim = arg[0];
    char tmp[256];
    char *p1, *p2, *p3;
    nd_addr a;
    int af;

    if (!delim) {
        reply(s->ctrl, 501, "Bad EPRT");
        return;
    }
    nd_strlcpy(tmp, arg + 1, sizeof(tmp));
    p1 = strchr(tmp, delim);
    if (!p1) { reply(s->ctrl, 501, "Bad EPRT"); return; }
    *p1 = '\0';
    af = atoi(tmp);
    p2 = p1 + 1;
    p3 = strchr(p2, delim);
    if (!p3) { reply(s->ctrl, 501, "Bad EPRT"); return; }
    *p3 = '\0';
    {
        char *p4 = strchr(p3 + 1, delim);
        int port;
        if (!p4) { reply(s->ctrl, 501, "Bad EPRT"); return; }
        *p4 = '\0';
        port = atoi(p3 + 1);
        if (nd_addr_parse(p2, 0, &a) != 0 || port <= 0) {
            reply(s->ctrl, 501, "Bad EPRT");
            return;
        }
        a.port = (uint16_t)port;
        if ((af == 1 && a.family != AF_INET) || (af == 2 && a.family != AF_INET6)) {
            reply(s->ctrl, 522, "Network protocol not supported");
            return;
        }
        if (!nd_addr_equal(&a, &s->peer)) {
            nd_addr an = a;
            nd_addr pn = s->peer;
            nd_addr_normalize(&an);
            nd_addr_normalize(&pn);
            if (!nd_addr_equal(&an, &pn)) {
                reply(s->ctrl, 501, "EPRT address must match control peer");
                return;
            }
        }
        close_passive(s);
        s->active = a;
        reply(s->ctrl, 200, "EPRT command successful");
    }
}

static void session_loop(ftp_sess *s)
{
    char line[4096];

    reply(s->ctrl, 220, "NetDebug FTP server ready");
    while (read_line(s->ctrl, line, sizeof(line)) == 0) {
        char *sp = strchr(line, ' ');
        char cmd[16];
        const char *arg = "";
        if (sp) {
            size_t n = (size_t)(sp - line);
            if (n >= sizeof(cmd))
                n = sizeof(cmd) - 1;
            memcpy(cmd, line, n);
            cmd[n] = '\0';
            arg = sp + 1;
        } else {
            nd_strlcpy(cmd, line, sizeof(cmd));
        }
        for (char *p = cmd; *p; p++)
            *p = (char)toupper((unsigned char)*p);

        if (!strcmp(cmd, "USER")) {
            if (s->cfg.anonymous && (!strcasecmp(arg, "anonymous") || !strcasecmp(arg, "ftp"))) {
                s->authed = 1;
                reply(s->ctrl, 230, "Anonymous login ok");
            } else if (s->cfg.user[0] && !strcmp(arg, s->cfg.user)) {
                reply(s->ctrl, 331, "Password required");
            } else {
                reply(s->ctrl, 530, "Login incorrect");
            }
        } else if (!strcmp(cmd, "PASS")) {
            if (s->authed) {
                reply(s->ctrl, 230, "Already logged in");
            } else if (s->cfg.pass[0] && !strcmp(arg, s->cfg.pass)) {
                s->authed = 1;
                reply(s->ctrl, 230, "Login ok");
            } else {
                reply(s->ctrl, 530, "Login incorrect");
            }
        } else if (!strcmp(cmd, "QUIT")) {
            reply(s->ctrl, 221, "Bye");
            break;
        } else if (!strcmp(cmd, "SYST")) {
            reply(s->ctrl, 215, "UNIX Type: L8");
        } else if (!strcmp(cmd, "FEAT")) {
            const char *feat =
                "211-Features:\r\n EPSV\r\n PASV\r\n SIZE\r\n MDTM\r\n MLSD\r\n UTF8\r\n211 End\r\n";
            nd_net_send_all(s->ctrl, feat, strlen(feat), 10000);
        } else if (!strcmp(cmd, "OPTS")) {
            reply(s->ctrl, 200, "UTF8 ok");
        } else if (!strcmp(cmd, "NOOP")) {
            reply(s->ctrl, 200, "OK");
        } else if (!strcmp(cmd, "TYPE")) {
            s->ascii = arg[0] == 'A' || arg[0] == 'a';
            reply(s->ctrl, 200, "Type set");
        } else if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
            if (need_auth(s)) continue;
            reply(s->ctrl, 257, "\"%s\"", s->cwd);
        } else if (!strcmp(cmd, "CWD")) {
            char virt[1024], real[1200];
            long long sz; time_t mt; int isdir;
            if (need_auth(s)) continue;
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
            if (file_stat(real, &sz, &mt, &isdir) == 0 && isdir) {
                nd_strlcpy(s->cwd, virt, sizeof(s->cwd));
                reply(s->ctrl, 250, "Directory changed");
            } else {
                reply(s->ctrl, 550, "No such directory");
            }
        } else if (!strcmp(cmd, "CDUP")) {
            char virt[1024], real[1200];
            long long sz; time_t mt; int isdir;
            if (need_auth(s)) continue;
            virt_norm(s->cwd, "..", virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
            if (file_stat(real, &sz, &mt, &isdir) == 0 && isdir) {
                nd_strlcpy(s->cwd, virt, sizeof(s->cwd));
                reply(s->ctrl, 250, "Directory changed");
            } else {
                reply(s->ctrl, 550, "No such directory");
            }
        } else if (!strcmp(cmd, "PASV")) {
            char err[128];
            if (need_auth(s)) continue;
            if (s->local.family != AF_INET) {
                reply(s->ctrl, 522, "Use EPSV for IPv6");
                continue;
            }
            if (open_passive(s, 0, err, sizeof(err)) != 0) {
                reply(s->ctrl, 425, "%s", err);
                continue;
            }
            {
                char ip[64];
                const char *adv = s->cfg.advertise[0] ? s->cfg.advertise : NULL;
                if (adv)
                    nd_strlcpy(ip, adv, sizeof(ip));
                else
                    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                             s->local.raw[0], s->local.raw[1], s->local.raw[2], s->local.raw[3]);
                reply(s->ctrl, 227, "Entering Passive Mode (%s,%u,%u)", ip,
                      s->pasv_local.port >> 8, s->pasv_local.port & 255);
            }
        } else if (!strcmp(cmd, "EPSV")) {
            char err[128];
            if (need_auth(s)) continue;
            if (open_passive(s, 1, err, sizeof(err)) != 0) {
                reply(s->ctrl, 425, "%s", err);
                continue;
            }
            reply(s->ctrl, 229, "Entering Extended Passive Mode (|||%u|)", s->pasv_local.port);
        } else if (!strcmp(cmd, "PORT")) {
            if (need_auth(s)) continue;
            handle_port(s, arg);
        } else if (!strcmp(cmd, "EPRT")) {
            if (need_auth(s)) continue;
            handle_eprt(s, arg);
        } else if (!strcmp(cmd, "LIST") || !strcmp(cmd, "NLST") || !strcmp(cmd, "MLSD")) {
            char virt[1024], real[1200];
            long long sz; time_t mt; int isdir;
            int dfd;
            char err[128];
            if (need_auth(s)) continue;
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
            if (file_stat(real, &sz, &mt, &isdir) != 0 || !isdir) {
                reply(s->ctrl, 550, "Not a directory");
                continue;
            }
            dfd = wait_data(s, err, sizeof(err));
            if (dfd < 0) {
                reply(s->ctrl, 425, "%s", err);
                continue;
            }
            send_list(s, dfd, virt, !strcmp(cmd, "MLSD"), !strcmp(cmd, "NLST"));
        } else if (!strcmp(cmd, "RETR")) {
            char virt[1024], real[1200];
            long long sz; time_t mt; int isdir;
            if (need_auth(s)) continue;
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
            if (file_stat(real, &sz, &mt, &isdir) != 0 || isdir) {
                reply(s->ctrl, 550, "Not a file");
                continue;
            }
            do_retr(s, virt);
        } else if (!strcmp(cmd, "STOR") || !strcmp(cmd, "APPE")) {
            char virt[1024];
            if (need_auth(s)) continue;
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            if (s->cfg.readonly) {
                reply(s->ctrl, 550, "Read-only server");
                continue;
            }
            do_stor(s, virt, !strcmp(cmd, "APPE"));
        } else if (!strcmp(cmd, "DELE")) {
            char virt[1024], real[1200];
            if (need_auth(s)) continue;
            if (s->cfg.readonly) { reply(s->ctrl, 550, "Read-only server"); continue; }
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
#ifdef _WIN32
            if (DeleteFileA(real)) reply(s->ctrl, 250, "Deleted");
            else reply(s->ctrl, 550, "Delete failed");
#else
            if (unlink(real) == 0) reply(s->ctrl, 250, "Deleted");
            else reply(s->ctrl, 550, "Delete failed");
#endif
        } else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
            char virt[1024], real[1200];
            if (need_auth(s)) continue;
            if (s->cfg.readonly) { reply(s->ctrl, 550, "Read-only server"); continue; }
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
#ifdef _WIN32
            if (CreateDirectoryA(real, NULL)) reply(s->ctrl, 257, "\"%s\" created", virt);
            else reply(s->ctrl, 550, "Create failed");
#else
            if (mkdir(real, 0755) == 0) reply(s->ctrl, 257, "\"%s\" created", virt);
            else reply(s->ctrl, 550, "Create failed");
#endif
        } else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
            char virt[1024], real[1200];
            if (need_auth(s)) continue;
            if (s->cfg.readonly) { reply(s->ctrl, 550, "Read-only server"); continue; }
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
#ifdef _WIN32
            if (RemoveDirectoryA(real)) reply(s->ctrl, 250, "Removed");
            else reply(s->ctrl, 550, "Remove failed");
#else
            if (rmdir(real) == 0) reply(s->ctrl, 250, "Removed");
            else reply(s->ctrl, 550, "Remove failed");
#endif
        } else if (!strcmp(cmd, "RNFR")) {
            char virt[1024], real[1200];
            if (need_auth(s)) continue;
            if (s->cfg.readonly) { reply(s->ctrl, 550, "Read-only server"); continue; }
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
            nd_strlcpy(s->rnfr, real, sizeof(s->rnfr));
            reply(s->ctrl, 350, "Ready for RNTO");
        } else if (!strcmp(cmd, "RNTO")) {
            char virt[1024], real[1200];
            if (need_auth(s)) continue;
            if (!s->rnfr[0]) { reply(s->ctrl, 503, "RNFR required"); continue; }
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
#ifdef _WIN32
            if (MoveFileA(s->rnfr, real)) reply(s->ctrl, 250, "Renamed");
            else reply(s->ctrl, 550, "Rename failed");
#else
            if (rename(s->rnfr, real) == 0) reply(s->ctrl, 250, "Renamed");
            else reply(s->ctrl, 550, "Rename failed");
#endif
            s->rnfr[0] = '\0';
        } else if (!strcmp(cmd, "SIZE")) {
            char virt[1024], real[1200];
            long long sz; time_t mt; int isdir;
            if (need_auth(s)) continue;
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
            if (file_stat(real, &sz, &mt, &isdir) == 0 && !isdir)
                reply(s->ctrl, 213, "%lld", sz);
            else
                reply(s->ctrl, 550, "Not a file");
        } else if (!strcmp(cmd, "MDTM")) {
            char virt[1024], real[1200];
            long long sz; time_t mt; int isdir;
            if (need_auth(s)) continue;
            virt_norm(s->cwd, arg, virt, sizeof(virt));
            real_path(s, virt, real, sizeof(real));
            if (file_stat(real, &sz, &mt, &isdir) == 0) {
                char t[32];
                fmt_modify(mt, t, sizeof(t));
                reply(s->ctrl, 213, "%s", t);
            } else {
                reply(s->ctrl, 550, "Not found");
            }
        } else if (!strcmp(cmd, "AUTH")) {
            reply(s->ctrl, 534, "TLS not supported");
        } else {
            reply(s->ctrl, 502, "Command not implemented");
        }
    }
}

static void *session_thread(void *arg)
{
    ftp_sess *s = (ftp_sess *)arg;
    session_loop(s);
    close_passive(s);
    nd_net_close(s->ctrl);
    free(s);
    return NULL;
}

int nd_ftp_server_start(int fd, const nd_addr *peer, const nd_addr *local,
                        const nd_ftp_srv_cfg *cfg)
{
    ftp_sess *s = (ftp_sess *)calloc(1, sizeof(*s));
    nd_thread th;
    if (!s)
        return -1;
    s->ctrl = fd;
    s->peer = *peer;
    s->local = *local;
    s->cfg = *cfg;
    s->pasv_fd = -1;
    nd_strlcpy(s->cwd, "/", sizeof(s->cwd));
    if (nd_thread_start(&th, session_thread, s, 512 * 1024) != 0) {
        free(s);
        return -1;
    }
    nd_thread_detach(&th);
    return 0;
}
