#include "proto/ftp_client.h"
#include "net/resolver.h"
#include "core/error.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "platform/netio.h"
#include "platform/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nd_ftp {
    int      ctrl;
    nd_addr  peer;
    nd_addr  local;
    nd_mutex lock;
    char     pwd[1024];
};

typedef struct {
    char *p;
    size_t len, cap;
} ftp_buf;

static int fbuf_put(ftp_buf *b, const void *data, size_t len)
{
    if (b->len + len + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 8192;
        while (ncap < b->len + len + 1)
            ncap *= 2;
        char *np = (char *)realloc(b->p, ncap);
        if (!np)
            return -1;
        b->p = np;
        b->cap = ncap;
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
    b->p[b->len] = '\0';
    return 0;
}

static int ctrl_read_line(nd_ftp *f, char *buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        char c;
        int r = nd_net_recv_some(f->ctrl, &c, 1, 15000);
        if (r == -2) {
            snprintf(buf, cap, "控制连接超时");
            return -1;
        }
        if (r <= 0) {
            snprintf(buf, cap, "控制连接已断开");
            return -1;
        }
        if (c == '\n') {
            if (n && buf[n - 1] == '\r')
                n--;
            buf[n] = '\0';
            return 0;
        }
        if (n + 1 < cap)
            buf[n++] = c;
    }
}

/* 读取一条完整应答（处理多行 123-...123 ） */
static int read_reply(nd_ftp *f, int *code, char *text, size_t cap)
{
    char line[2048];
    code_t:
    if (ctrl_read_line(f, line, sizeof(line)) != 0) {
        if (text)
            snprintf(text, cap, "%s", line);
        return -1;
    }
    if (strlen(line) < 4 || line[0] < '1' || line[0] > '5')
        goto code_t;
    *code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    if (text)
        nd_strlcpy(text, line, cap);
    if (line[3] == '-') {
        char end[8];
        snprintf(end, sizeof(end), "%03d ", *code);
        for (;;) {
            if (ctrl_read_line(f, line, sizeof(line)) != 0)
                return -1;
            if (strncmp(line, end, 4) == 0) {
                if (text) {
                    size_t used = strlen(text);
                    if (used + 1 < cap)
                        snprintf(text + used, cap - used, " | %s", line);
                }
                break;
            }
        }
    }
    return 0;
}

static int send_line(nd_ftp *f, const char *fmt, ...)
{
    char buf[3072];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n > sizeof(buf) - 3)
        n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = '\0';
    return nd_net_send_all(f->ctrl, buf, (size_t)n, 15000);
}

static int cmd_simple(nd_ftp *f, int expect, char *err, size_t errlen,
                      const char *fmt, ...)
{
    char line[3072];
    char text[2048] = "";
    int code = 0;
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return ND_E_PARAM;
    if (send_line(f, "%s", line) != 0) {
        snprintf(err, errlen, "发送命令失败");
        return ND_E_IO;
    }
    if (read_reply(f, &code, text, sizeof(text)) != 0) {
        snprintf(err, errlen, "%s", text);
        return ND_E_IO;
    }
    if (expect && code != expect) {
        snprintf(err, errlen, "%s", text);
        return ND_E_PROTO;
    }
    return ND_OK;
}

static int open_data(nd_ftp *f, int *out, char *err, size_t errlen)
{
    char text[2048] = "";
    int code = 0;
    int fd;
    nd_addr target;
    nd_addr bindaddr;

    *out = -1;
    memset(&target, 0, sizeof(target));

    /* 优先 EPSV（IPv4/IPv6 通用） */
    if (send_line(f, "EPSV") == 0 && read_reply(f, &code, text, sizeof(text)) == 0 &&
        code == 229) {
        const char *p = strchr(text, '(');
        int port = 0;
        if (p && sscanf(p, "(|||%d|)", &port) == 1 && port > 0 && port < 65536) {
            target = f->peer;
            target.port = (uint16_t)port;
        }
    }

    if (target.family == 0) {
        if (send_line(f, "PASV") != 0 || read_reply(f, &code, text, sizeof(text)) != 0 ||
            code != 227) {
            snprintf(err, errlen, "服务器拒绝被动模式: %s", text);
            return ND_E_PROTO;
        }
        {
            int h1, h2, h3, h4, p1, p2;
            const char *p = strchr(text, '(');
            if (!p || sscanf(p, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
                snprintf(err, errlen, "无法解析 PASV 应答: %s", text);
                return ND_E_PROTO;
            }
            /* 以控制连接对端地址为准，规避服务器回私网地址 */
            target = f->peer;
            target.port = (uint16_t)(p1 * 256 + p2);
        }
    }

    fd = nd_net_socket(target.family, SOCK_STREAM);
    if (fd < 0) {
        snprintf(err, errlen, "创建数据连接失败");
        return ND_E_IO;
    }
    bindaddr = f->local;
    bindaddr.port = 0;
    if (nd_net_bind(fd, &bindaddr) != 0) {
        nd_net_close(fd);
        snprintf(err, errlen, "数据连接绑定本地地址失败");
        return ND_E_IO;
    }
    {
        int se = 0;
        if (nd_net_connect(fd, &target, 8000, &se) != 0) {
            nd_net_close(fd);
            snprintf(err, errlen, "数据连接失败");
            return ND_E_IO;
        }
    }
    nd_net_set_timeouts(fd, 15000, 15000);
    *out = fd;
    return ND_OK;
}

nd_ftp *nd_ftp_connect(const char *host, uint16_t port, nd_family family,
                       int timeout_ms, char *err, size_t errlen)
{
    nd_addr addrs[16];
    size_t n = 0;
    int fd = -1;
    int sys = 0;
    size_t used = 0;
    nd_ftp *f;

    if (nd_resolve(host, port, family, addrs, 16, &n, err, errlen) != 0)
        return NULL;

    for (size_t i = 0; i < n; i++) {
        fd = nd_net_socket(addrs[i].family, SOCK_STREAM);
        if (fd < 0)
            continue;
        if (nd_net_connect(fd, &addrs[i], timeout_ms, &sys) == 0) {
            used = i;
            break;
        }
        nd_net_close(fd);
        fd = -1;
    }
    if (fd < 0) {
        snprintf(err, errlen, "连接 FTP 服务器失败");
        return NULL;
    }
    nd_net_set_timeouts(fd, 15000, 15000);
    nd_net_set_nodelay(fd);

    f = (nd_ftp *)calloc(1, sizeof(*f));
    if (!f) {
        nd_net_close(fd);
        return NULL;
    }
    f->ctrl = fd;
    f->peer = addrs[used];
    nd_net_getsockname(fd, &f->local);
    nd_mutex_init(&f->lock);
    nd_strlcpy(f->pwd, "/", sizeof(f->pwd));

    {
        char text[2048] = "";
        int code = 0;
        if (read_reply(f, &code, text, sizeof(text)) != 0 || code != 220) {
            snprintf(err, errlen, "FTP 欢迎信息异常: %s", text);
            nd_ftp_close(f);
            return NULL;
        }
    }
    return f;
}

int nd_ftp_login(nd_ftp *f, const char *user, const char *pass,
                 char *err, size_t errlen)
{
    char text[2048] = "";
    int code = 0;

    nd_mutex_lock(&f->lock);
    if (send_line(f, "USER %s", user && user[0] ? user : "anonymous") != 0 ||
        read_reply(f, &code, text, sizeof(text)) != 0) {
        nd_mutex_unlock(&f->lock);
        snprintf(err, errlen, "USER 命令失败");
        return ND_E_IO;
    }
    if (code == 331) {
        if (send_line(f, "PASS %s", pass ? pass : "") != 0 ||
            read_reply(f, &code, text, sizeof(text)) != 0) {
            nd_mutex_unlock(&f->lock);
            snprintf(err, errlen, "PASS 命令失败");
            return ND_E_IO;
        }
    }
    nd_mutex_unlock(&f->lock);
    if (code != 230 && code != 202) {
        snprintf(err, errlen, "登录失败: %s", text);
        return ND_E_PROTO_AUTH;
    }
    return ND_OK;
}

void nd_ftp_close(nd_ftp *f)
{
    if (!f)
        return;
    if (f->ctrl >= 0) {
        nd_net_shutdown(f->ctrl);
        nd_net_close(f->ctrl);
        f->ctrl = -1;
    }
    nd_mutex_destroy(&f->lock);
    free(f);
}

int nd_ftp_pwd(nd_ftp *f, char *out, size_t cap, char *err, size_t errlen)
{
    char text[2048] = "";
    int code = 0;
    int rc = ND_E_PROTO;

    nd_mutex_lock(&f->lock);
    if (send_line(f, "PWD") != 0 || read_reply(f, &code, text, sizeof(text)) != 0) {
        snprintf(err, errlen, "PWD 命令失败");
        rc = ND_E_IO;
        goto out;
    }
    if (code != 257) {
        snprintf(err, errlen, "%s", text);
        goto out;
    }
    {
        const char *a = strchr(text, '"');
        const char *b = a ? strchr(a + 1, '"') : NULL;
        if (a && b) {
            size_t len = (size_t)(b - a - 1);
            if (len >= cap)
                len = cap - 1;
            memcpy(out, a + 1, len);
            out[len] = '\0';
            nd_strlcpy(f->pwd, out, sizeof(f->pwd));
            rc = ND_OK;
        } else {
            snprintf(err, errlen, "无法解析 PWD: %s", text);
        }
    }
out:
    nd_mutex_unlock(&f->lock);
    return rc;
}

int nd_ftp_cwd(nd_ftp *f, const char *path, char *err, size_t errlen)
{
    int rc;
    nd_mutex_lock(&f->lock);
    rc = cmd_simple(f, 250, err, errlen, "CWD %s", path);
    nd_mutex_unlock(&f->lock);
    return rc;
}

static int parse_mlsd(const char *data, size_t len, nd_ftp_entry **out, size_t *n)
{
    nd_ftp_entry *arr = NULL;
    size_t cnt = 0, cap = 0;
    const char *p = data;

    while (p && *p && (size_t)(p - data) < len) {
        const char *eol = strstr(p, "\r\n");
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char line[2048];
        if (llen >= sizeof(line))
            llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        line[llen] = '\0';

        {
            char *space = strchr(line, ' ');
            if (!space) {
                p = eol ? eol + 2 : NULL;
                continue;
            }
            *space = '\0';
            {
                char *name = space + 1;
                nd_ftp_entry ent;
                memset(&ent, 0, sizeof(ent));
                nd_strlcpy(ent.name, name, sizeof(ent.name));
                for (char *tok = strtok(line, ";"); tok; tok = strtok(NULL, ";")) {
                    if (!strncmp(tok, "type=dir", 8))
                        ent.is_dir = 1;
                    else if (!strncmp(tok, "type=cdir", 9) || !strncmp(tok, "type=pdir", 9)) {
                        ent.is_dir = 1;
                        if (!strcmp(name, ".") || !strcmp(name, ".."))
                            ent.name[0] = '\0';
                    } else if (!strncmp(tok, "size=", 5)) {
                        ent.size = atoll(tok + 5);
                    } else if (!strncmp(tok, "modify=", 7)) {
                        nd_strlcpy(ent.modify, tok + 7, sizeof(ent.modify));
                    }
                }
                if (ent.name[0]) {
                    if (cnt == cap) {
                        size_t ncap = cap ? cap * 2 : 32;
                        nd_ftp_entry *na = (nd_ftp_entry *)realloc(arr, ncap * sizeof(*na));
                        if (!na) {
                            free(arr);
                            return -1;
                        }
                        arr = na;
                        cap = ncap;
                    }
                    arr[cnt++] = ent;
                }
            }
        }
        p = eol ? eol + 2 : NULL;
    }
    *out = arr;
    *n = cnt;
    return 0;
}

static int parse_list(const char *data, size_t len, nd_ftp_entry **out, size_t *n)
{
    nd_ftp_entry *arr = NULL;
    size_t cnt = 0, cap = 0;
    const char *p = data;

    while (p && *p && (size_t)(p - data) < len) {
        const char *eol = strstr(p, "\r\n");
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char line[2048];
        char *q;
        char *name = NULL;
        int field = 0;
        nd_ftp_entry ent;

        if (llen >= sizeof(line))
            llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        line[llen] = '\0';
        memset(&ent, 0, sizeof(ent));

        q = line;
        while (*q) {
            char *tok;
            while (*q == ' ')
                q++;
            if (!*q)
                break;
            if (field == 8) {
                name = q;
                while (*q)
                    q++;
                break;
            }
            tok = q;
            while (*q && *q != ' ')
                q++;
            if (*q)
                *q++ = '\0';
            if (field == 0) {
                ent.is_dir = tok[0] == 'd';
                nd_strlcpy(ent.perms, tok, sizeof(ent.perms));
            } else if (field == 4) {
                ent.size = atoll(tok);
            } else if (field == 5) {
                nd_strlcpy(ent.modify, tok, sizeof(ent.modify));
            }
            field++;
        }
        if (name && *name) {
            nd_strlcpy(ent.name, name, sizeof(ent.name));
            if (cnt == cap) {
                size_t ncap = cap ? cap * 2 : 32;
                nd_ftp_entry *na = (nd_ftp_entry *)realloc(arr, ncap * sizeof(*na));
                if (!na) {
                    free(arr);
                    return -1;
                }
                arr = na;
                cap = ncap;
            }
            arr[cnt++] = ent;
        }
        p = eol ? eol + 2 : NULL;
    }
    *out = arr;
    *n = cnt;
    return 0;
}

int nd_ftp_list(nd_ftp *f, const char *path, nd_ftp_entry **out, size_t *n,
                char *err, size_t errlen)
{
    int rc = ND_E_PROTO;
    int datafd = -1;
    int code = 0;
    char text[2048] = "";
    ftp_buf buf = {0};
    int mlsd = 1;

    *out = NULL;
    *n = 0;
    nd_mutex_lock(&f->lock);

    if (cmd_simple(f, 200, err, errlen, "TYPE I") != ND_OK) {
        rc = ND_E_PROTO;
        goto done;
    }

retry:
    if (open_data(f, &datafd, err, errlen) != ND_OK) {
        rc = ND_E_IO;
        goto done;
    }
    if (send_line(f, "%s %s", mlsd ? "MLSD" : "LIST",
                  path && path[0] ? path : "") != 0) {
        snprintf(err, errlen, "发送列表命令失败");
        rc = ND_E_IO;
        goto done;
    }
    if (read_reply(f, &code, text, sizeof(text)) != 0) {
        snprintf(err, errlen, "读取列表应答失败");
        rc = ND_E_IO;
        goto done;
    }
    if (code >= 400) {
        nd_net_close(datafd);
        datafd = -1;
        if (mlsd) {
            mlsd = 0;
            goto retry;
        }
        snprintf(err, errlen, "%s", text);
        rc = ND_E_PROTO;
        goto done;
    }

    for (;;) {
        char tmp[8192];
        int r = nd_net_recv_some(datafd, tmp, sizeof(tmp), 15000);
        if (r > 0) {
            if (fbuf_put(&buf, tmp, (size_t)r) != 0) {
                rc = ND_E_INTERNAL;
                goto done;
            }
            continue;
        }
        if (r == -2) {
            snprintf(err, errlen, "读取目录超时");
            rc = ND_E_IO;
            goto done;
        }
        break;
    }
    nd_net_close(datafd);
    datafd = -1;
    if (read_reply(f, &code, text, sizeof(text)) != 0 || (code != 226 && code != 250)) {
        snprintf(err, errlen, "列表传输未正常结束: %s", text);
        rc = ND_E_PROTO;
        goto done;
    }

    if (mlsd)
        rc = parse_mlsd(buf.p ? buf.p : "", buf.len, out, n) == 0 ? ND_OK : ND_E_INTERNAL;
    else
        rc = parse_list(buf.p ? buf.p : "", buf.len, out, n) == 0 ? ND_OK : ND_E_INTERNAL;

done:
    if (datafd >= 0)
        nd_net_close(datafd);
    free(buf.p);
    nd_mutex_unlock(&f->lock);
    return rc;
}

int nd_ftp_mkdir(nd_ftp *f, const char *path, char *err, size_t errlen)
{
    int rc;
    nd_mutex_lock(&f->lock);
    rc = cmd_simple(f, 257, err, errlen, "MKD %s", path);
    nd_mutex_unlock(&f->lock);
    return rc;
}

int nd_ftp_rmdir(nd_ftp *f, const char *path, char *err, size_t errlen)
{
    int rc;
    nd_mutex_lock(&f->lock);
    rc = cmd_simple(f, 250, err, errlen, "RMD %s", path);
    nd_mutex_unlock(&f->lock);
    return rc;
}

int nd_ftp_delete(nd_ftp *f, const char *path, char *err, size_t errlen)
{
    int rc;
    nd_mutex_lock(&f->lock);
    rc = cmd_simple(f, 250, err, errlen, "DELE %s", path);
    nd_mutex_unlock(&f->lock);
    return rc;
}

int nd_ftp_rename(nd_ftp *f, const char *from, const char *to,
                  char *err, size_t errlen)
{
    int rc;
    nd_mutex_lock(&f->lock);
    rc = cmd_simple(f, 350, err, errlen, "RNFR %s", from);
    if (rc == ND_OK)
        rc = cmd_simple(f, 250, err, errlen, "RNTO %s", to);
    nd_mutex_unlock(&f->lock);
    return rc;
}

static int transfer_open_cmd(nd_ftp *f, int datafd, const char *cmd,
                             char *err, size_t errlen)
{
    char text[2048] = "";
    int code = 0;
    if (send_line(f, "%s", cmd) != 0) {
        snprintf(err, errlen, "发送传输命令失败");
        return ND_E_IO;
    }
    if (read_reply(f, &code, text, sizeof(text)) != 0) {
        snprintf(err, errlen, "读取传输应答失败");
        return ND_E_IO;
    }
    if (code != 150 && code != 125) {
        nd_net_close(datafd);
        snprintf(err, errlen, "%s", text);
        return ND_E_PROTO;
    }
    return ND_OK;
}

int nd_ftp_download(nd_ftp *f, const char *remote, const char *local,
                    uint64_t *bytes, char *err, size_t errlen)
{
    int datafd = -1;
    int code = 0;
    char text[2048] = "";
    FILE *fp = NULL;
    uint64_t total = 0;
    int rc = ND_E_IO;
    char cmd[3072];

    *bytes = 0;
    nd_mutex_lock(&f->lock);
    if (cmd_simple(f, 0, err, errlen, "TYPE I") != ND_OK)
        goto done;
    if (open_data(f, &datafd, err, errlen) != ND_OK)
        goto done;
    snprintf(cmd, sizeof(cmd), "RETR %s", remote);
    if (transfer_open_cmd(f, datafd, cmd, err, errlen) != ND_OK) {
        datafd = -1; /* 已被关闭 */
        goto done;
    }
    fp = fopen(local, "wb");
    if (!fp) {
        snprintf(err, errlen, "无法写入本地文件: %s", local);
        nd_net_close(datafd);
        datafd = -1;
        goto done;
    }
    for (;;) {
        char buf[65536];
        int r = nd_net_recv_some(datafd, buf, sizeof(buf), 15000);
        if (r > 0) {
            if (fwrite(buf, 1, (size_t)r, fp) != (size_t)r) {
                snprintf(err, errlen, "写入本地文件失败");
                goto done;
            }
            total += (uint64_t)r;
            continue;
        }
        if (r == -2) {
            snprintf(err, errlen, "下载超时（已收 %llu 字节）", (unsigned long long)total);
            goto done;
        }
        break;
    }
    fclose(fp);
    fp = NULL;
    nd_net_close(datafd);
    datafd = -1;
    if (read_reply(f, &code, text, sizeof(text)) != 0 || (code != 226 && code != 250)) {
        snprintf(err, errlen, "下载未正常结束: %s", text);
        goto done;
    }
    *bytes = total;
    rc = ND_OK;

done:
    if (fp)
        fclose(fp);
    if (datafd >= 0)
        nd_net_close(datafd);
    nd_mutex_unlock(&f->lock);
    return rc;
}

int nd_ftp_upload(nd_ftp *f, const char *local, const char *remote,
                  uint64_t *bytes, char *err, size_t errlen)
{
    int datafd = -1;
    int code = 0;
    char text[2048] = "";
    FILE *fp = NULL;
    uint64_t total = 0;
    int rc = ND_E_IO;
    char cmd[3072];

    *bytes = 0;
    nd_mutex_lock(&f->lock);
    if (cmd_simple(f, 0, err, errlen, "TYPE I") != ND_OK)
        goto done;
    if (open_data(f, &datafd, err, errlen) != ND_OK)
        goto done;
    snprintf(cmd, sizeof(cmd), "STOR %s", remote);
    if (transfer_open_cmd(f, datafd, cmd, err, errlen) != ND_OK) {
        datafd = -1;
        goto done;
    }
    fp = fopen(local, "rb");
    if (!fp) {
        snprintf(err, errlen, "无法读取本地文件: %s", local);
        nd_net_close(datafd);
        datafd = -1;
        goto done;
    }
    for (;;) {
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0)
            break;
        if (nd_net_send_all(datafd, buf, n, 15000) != 0) {
            snprintf(err, errlen, "上传数据失败");
            goto done;
        }
        total += n;
    }
    fclose(fp);
    fp = NULL;
    nd_net_close(datafd);
    datafd = -1;
    if (read_reply(f, &code, text, sizeof(text)) != 0 || (code != 226 && code != 250)) {
        snprintf(err, errlen, "上传未正常结束: %s", text);
        goto done;
    }
    *bytes = total;
    rc = ND_OK;

done:
    if (fp)
        fclose(fp);
    if (datafd >= 0)
        nd_net_close(datafd);
    nd_mutex_unlock(&f->lock);
    return rc;
}
