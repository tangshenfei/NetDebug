#include "proto/http_server.h"

#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "httpd/web_assets.h"
#include "net/addr.h"
#include "platform/netio.h"
#include "session/service.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

typedef struct {
    int             fd;
    uint64_t        service_id;
    nd_service     *svc;
    nd_http_srv_cfg cfg;
} http_job;

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int wait_readable(int fd, int timeout_ms)
{
    nd_pollfd p;
    int r;

    p.fd = fd;
    p.events = ND_POLLIN;
    p.revents = 0;
    r = nd_net_poll(&p, 1, timeout_ms);
    return r > 0 && (p.revents & ND_POLLIN);
}

/* 读取到请求头结束（\r\n\r\n）。返回 1 成功，0 失败。 */
static int read_request(int fd, char *buf, size_t cap, size_t *out_len)
{
    uint64_t deadline = nd_monotonic_ms() + 5000;
    size_t used = 0;

    while (used + 1 < cap) {
        int remain = 0;
        int n;
        uint64_t now = nd_monotonic_ms();

        if (now >= deadline)
            return 0;
        remain = (int)(deadline - now);
        if (remain > 1000)
            remain = 1000;
        if (!wait_readable(fd, remain))
            continue;
        n = nd_net_recv_some(fd, buf + used, cap - 1 - used, 0);
        if (n <= 0)
            return 0;
        used += (size_t)n;
        buf[used] = '\0';
        if (used >= 4 && strstr(buf, "\r\n\r\n")) {
            *out_len = used;
            return 1;
        }
    }
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    while (*in) {
        if (*in == '%') {
            int a = hexval((unsigned char)in[1]);
            int b = hexval((unsigned char)in[2]);
            if (a < 0 || b < 0)
                return -1;
            if ((a == 0 && b == 0))
                return -1;
            if (o + 1 >= cap)
                return -1;
            out[o++] = (char)((a << 4) | b);
            in += 3;
        } else {
            if (o + 1 >= cap)
                return -1;
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
    return 0;
}

/* 把 URL 路径规范化成 /a/b 形式，拒绝 .. 越界。 */
static int normalize_path(const char *target, char *out, size_t cap)
{
    char tmp[2048];
    char decoded[2048];
    const char *path = target;
    size_t o = 0;
    int last_slash = 1;

    if (!strncmp(path, "http://", 7) || !strncmp(path, "https://", 8)) {
        path = strstr(path, "://");
        path = path ? strchr(path + 3, '/') : NULL;
        if (!path)
            path = "/";
    }
    if (*path != '/')
        return -1;
    {
        size_t i = 0;
        while (path[i] && path[i] != '?' && path[i] != '#') {
            if (i + 1 >= sizeof(tmp))
                return -1;
            tmp[i] = path[i];
            i++;
        }
        tmp[i] = '\0';
    }
    if (url_decode(tmp, decoded, sizeof(decoded)) != 0)
        return -1;

    out[o++] = '/';
    for (size_t i = 0; decoded[i]; i++) {
        char c = decoded[i];
        if (c == '\\')
            c = '/';
        if (c == '/') {
            if (last_slash)
                continue;
            last_slash = 1;
            if (o + 1 >= cap)
                return -1;
            out[o++] = '/';
            continue;
        }
        last_slash = 0;
        if (o + 1 >= cap)
            return -1;
        out[o++] = c;
    }
    out[o] = '\0';

    /* 拒绝任何 .. 路径段 */
    {
        const char *p = out;
        while ((p = strstr(p, "..")) != NULL) {
            int left = p == out || p[-1] == '/';
            int right = p[2] == '\0' || p[2] == '/';
            if (left && right)
                return -1;
            p += 2;
        }
    }
    return 0;
}

static const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (ieq(dot, ".html") || ieq(dot, ".htm"))
        return "text/html; charset=utf-8";
    if (ieq(dot, ".css"))
        return "text/css; charset=utf-8";
    if (ieq(dot, ".js"))
        return "application/javascript; charset=utf-8";
    if (ieq(dot, ".json"))
        return "application/json; charset=utf-8";
    if (ieq(dot, ".txt") || ieq(dot, ".log"))
        return "text/plain; charset=utf-8";
    if (ieq(dot, ".svg"))
        return "image/svg+xml";
    if (ieq(dot, ".png"))
        return "image/png";
    if (ieq(dot, ".jpg") || ieq(dot, ".jpeg"))
        return "image/jpeg";
    if (ieq(dot, ".gif"))
        return "image/gif";
    if (ieq(dot, ".ico"))
        return "image/x-icon";
    if (ieq(dot, ".woff2"))
        return "font/woff2";
    if (ieq(dot, ".woff"))
        return "font/woff";
    if (ieq(dot, ".ttf"))
        return "font/ttf";
    if (ieq(dot, ".pdf"))
        return "application/pdf";
    return "application/octet-stream";
}

static int send_all(int fd, const char *data, size_t len)
{
    return nd_net_send_all(fd, data, len, 5000);
}

static int send_head(int fd, int status, const char *reason, const char *ctype,
                     long long length, int head_only)
{
    char head[768];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Server: NetDebug\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, ctype, length);
    if (n <= 0)
        return -1;
    (void)head_only;
    return send_all(fd, head, (size_t)n);
}

static void send_error(int fd, int status, const char *reason, const char *msg, int head_only)
{
    char body[512];
    int n = snprintf(body, sizeof(body), "%d %s\n%s\n", status, reason, msg);
    if (n < 0)
        n = 0;
    if (send_head(fd, status, reason, "text/plain; charset=utf-8", n, head_only) != 0)
        return;
    if (!head_only)
        send_all(fd, body, (size_t)n);
}

static void send_echo(int fd, const char *req, size_t len, int head_only)
{
    char *body;
    size_t cap = len + 128;
    int n;

    body = (char *)malloc(cap);
    if (!body) {
        send_error(fd, 500, "Internal Server Error", "out of memory", head_only);
        return;
    }
    n = snprintf(body, cap, "HTTP echo\n\n%s", req);
    if (n < 0)
        n = 0;
    if (send_head(fd, 200, "OK", "text/plain; charset=utf-8", n, head_only) == 0 && !head_only)
        send_all(fd, body, (size_t)n);
    free(body);
}

static void serve_file(int fd, const char *file, int head_only)
{
    struct stat st;
    FILE *f;
    char head[768];
    char buf[65536];
    size_t n;
    long long size;
    int hlen;

    if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_error(fd, 404, "Not Found", "file not found", head_only);
        return;
    }
    f = fopen(file, "rb");
    if (!f) {
        send_error(fd, 403, "Forbidden", "cannot open file", head_only);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        send_error(fd, 500, "Internal Server Error", "cannot seek file", head_only);
        return;
    }
    size = (long long)ftell(f);
    if (size < 0)
        size = 0;
    rewind(f);

    hlen = snprintf(head, sizeof(head),
                    "HTTP/1.1 200 OK\r\n"
                    "Server: NetDebug\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %lld\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    mime_type(file), size);
    if (hlen <= 0 || send_all(fd, head, (size_t)hlen) != 0) {
        fclose(f);
        return;
    }
    if (head_only) {
        fclose(f);
        return;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(fd, buf, n) != 0)
            break;
    }
    fclose(f);
}

typedef struct {
    char     *p;
    size_t    len;
    size_t    cap;
} hsbuf;

typedef struct {
    char      name[512];
    int       is_dir;
    long long size;
    time_t    mtime;
} h_entry;

static int hsbuf_add(hsbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        char *p;
        while (cap < b->len + n + 1)
            cap *= 2;
        p = (char *)realloc(b->p, cap);
        if (!p)
            return -1;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int hsbuf_puts(hsbuf *b, const char *s)
{
    return hsbuf_add(b, s, strlen(s));
}

static int hsbuf_printf(hsbuf *b, const char *fmt, ...)
{
    char tmp[4096];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= sizeof(tmp))
        n = (int)sizeof(tmp) - 1;
    return hsbuf_add(b, tmp, (size_t)n);
}

static int lower_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    while (*in && o + 4 < cap) {
        unsigned char c = (unsigned char)*in++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    while (*in && o + 1 < cap) {
        const char *rep = NULL;
        switch (*in) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (o + n >= cap)
                break;
            memcpy(out + o, rep, n);
            o += n;
        } else {
            out[o++] = *in;
        }
        in++;
    }
    out[o] = '\0';
}

static void format_size(long long n, char *out, size_t cap)
{
    if (n >= 1024LL * 1024 * 1024)
        snprintf(out, cap, "%.2f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
    else if (n >= 1024LL * 1024)
        snprintf(out, cap, "%.2f MB", (double)n / (1024.0 * 1024.0));
    else if (n >= 1024)
        snprintf(out, cap, "%.1f KB", (double)n / 1024.0);
    else
        snprintf(out, cap, "%lld B", n);
}

static void format_time(time_t t, char *out, size_t cap)
{
    struct tm tmv;
    if (t <= 0) {
        out[0] = '\0';
        return;
    }
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%Y-%m-%d %H:%M", &tmv);
}

static int h_entry_cmp(const void *a, const void *b)
{
    const h_entry *x = (const h_entry *)a;
    const h_entry *y = (const h_entry *)b;
    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;
    return lower_cmp(x->name, y->name);
}

static int add_entry(h_entry **arr, size_t *n, size_t *cap,
                     const char *name, int is_dir, long long size, time_t mtime)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        h_entry *p = (h_entry *)realloc(*arr, nc * sizeof(**arr));
        if (!p)
            return -1;
        *arr = p;
        *cap = nc;
    }
    nd_strlcpy((*arr)[*n].name, name, sizeof((*arr)[*n].name));
    (*arr)[*n].is_dir = is_dir;
    (*arr)[*n].size = size;
    (*arr)[*n].mtime = mtime;
    (*n)++;
    return 0;
}

static int list_dir(const char *dir, h_entry **out, size_t *count)
{
    h_entry *arr = NULL;
    size_t n = 0, cap = 0;

#ifdef _WIN32
    wchar_t wdir[2048], pat[2100];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, 2048) <= 0)
        return -1;
    _snwprintf(pat, 2100, L"%s\\*", wdir);
    pat[2099] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        char name[512];
        ULONGLONG ticks;
        time_t mt;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
                                (int)sizeof(name), NULL, NULL) <= 0)
            continue;
        ticks = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) |
                fd.ftLastWriteTime.dwLowDateTime;
        mt = (time_t)((ticks - 116444736000000000ULL) / 10000000ULL);
        if (add_entry(&arr, &n, &cap, name,
                      (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                      ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, mt) != 0) {
            FindClose(h);
            free(arr);
            return -1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        char full[2048];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (stat(full, &st) != 0)
            continue;
        if (add_entry(&arr, &n, &cap, de->d_name, S_ISDIR(st.st_mode),
                      (long long)st.st_size, st.st_mtime) != 0) {
            closedir(d);
            free(arr);
            return -1;
        }
    }
    closedir(d);
#endif

    if (n > 1)
        qsort(arr, n, sizeof(*arr), h_entry_cmp);
    else if (n == 0) {
        arr = NULL;
    }
    *out = arr;
    *count = n;
    return 0;
}

static int build_listing(const char *urlpath, const h_entry *ents, size_t n,
                         char **out, size_t *outlen)
{
    hsbuf b;
    char base[2048], parent[2048], epath[2048];
    size_t l;

    memset(&b, 0, sizeof(b));
    l = strlen(urlpath);
    if (l + 2 >= sizeof(base))
        return -1;
    memcpy(base, urlpath, l + 1);
    if (l == 0 || base[l - 1] != '/') {
        base[l] = '/';
        base[l + 1] = '\0';
    }
    parent[0] = '\0';
    if (strcmp(base, "/") != 0) {
        char tmp[2048];
        char *slash;
        nd_strlcpy(tmp, base, sizeof(tmp));
        l = strlen(tmp);
        if (l && tmp[l - 1] == '/')
            tmp[l - 1] = '\0';
        slash = strrchr(tmp, '/');
        if (slash)
            slash[1] = '\0';
        nd_strlcpy(parent, tmp[0] ? tmp : "/", sizeof(parent));
    }
    html_escape(urlpath, epath, sizeof(epath));

    hsbuf_puts(&b, "<!doctype html><meta charset=\"utf-8\"><title>Index of ");
    hsbuf_puts(&b, epath);
    hsbuf_puts(&b, "</title><style>body{background:#0f141c;color:#d7dee8;font:14px/1.6 'Segoe UI',sans-serif;padding:24px}"
                   "table{border-collapse:collapse;width:100%;max-width:1000px}a{color:#6ea8ff;text-decoration:none}"
                   "td{padding:6px 10px;border-bottom:1px solid #26303d}th{text-align:left;color:#8d9bad;padding:6px 10px}"
                   "h1{font-size:20px}</style><h1>Index of ");
    hsbuf_puts(&b, epath);
    hsbuf_puts(&b, "</h1><table><tr><th>名称</th><th>大小</th><th>修改时间</th></tr>");
    if (parent[0])
        hsbuf_printf(&b, "<tr><td><a href=\"%s\">../</a></td><td>-</td><td>-</td></tr>", parent);

    for (size_t i = 0; i < n; i++) {
        char href[1800], name[1400], size[32], date[64];
        url_encode(ents[i].name, href, sizeof(href));
        html_escape(ents[i].name, name, sizeof(name));
        if (ents[i].is_dir) {
            size_t hl = strlen(href);
            if (hl + 2 < sizeof(href)) {
                href[hl++] = '/';
                href[hl] = '\0';
            }
        }
        format_size(ents[i].size, size, sizeof(size));
        format_time(ents[i].mtime, date, sizeof(date));
        hsbuf_printf(&b, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>",
                     base, href, name, ents[i].is_dir ? "/" : "", size, date);
    }
    if (n == 0)
        hsbuf_puts(&b, "<tr><td colspan=\"3\">(空目录)</td></tr>");
    hsbuf_puts(&b, "</table>");
    if (!b.p)
        return -1;
    *out = b.p;
    *outlen = b.len;
    return 0;
}

static void serve_directory(int fd, const char *dir, const char *urlpath, int head_only)
{
    h_entry *ents = NULL;
    size_t n = 0;
    char *html = NULL;
    size_t len = 0;

    if (list_dir(dir, &ents, &n) != 0) {
        send_error(fd, 403, "Forbidden", "cannot list directory", head_only);
        return;
    }
    if (build_listing(urlpath, ents, n, &html, &len) != 0) {
        free(ents);
        send_error(fd, 500, "Internal Server Error", "listing failed", head_only);
        return;
    }
    free(ents);
    if (send_head(fd, 200, "OK", "text/html; charset=utf-8", (long long)len, head_only) == 0 &&
        !head_only)
        send_all(fd, html, len);
    free(html);
}

static const nd_web_asset *find_web_asset(const char *path)
{
    for (size_t i = 0; i < nd_web_assets_count; i++) {
        if (!strcmp(nd_web_assets[i].path, path))
            return &nd_web_assets[i];
    }
    return NULL;
}

static void speedtest_asset_path(const char *path, char *out, size_t cap)
{
    if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
        nd_strlcpy(out, "/speedtest/index.html", cap);
    else if (!strcmp(path, "/speedtest.js"))
        nd_strlcpy(out, "/speedtest/speedtest.js", cap);
    else if (!strcmp(path, "/speedtest_worker.js"))
        nd_strlcpy(out, "/speedtest/speedtest_worker.js", cap);
    else if (!strcmp(path, "/favicon.ico"))
        nd_strlcpy(out, "/speedtest/images/favicon.svg", cap);
    else if (!strcmp(path, "/server-list.json"))
        nd_strlcpy(out, "/speedtest/server-list.json", cap);
    else if (!strncmp(path, "/javascript/", 12) || !strncmp(path, "/styling/", 9) ||
             !strncmp(path, "/images/", 8) || !strncmp(path, "/fonts/", 7))
        snprintf(out, cap, "/speedtest%s", path);
    else
        snprintf(out, cap, "/speedtest%s", path);
}

static void send_web_asset(int fd, const nd_web_asset *a, int head_only)
{
    if (!a) {
        send_error(fd, 404, "Not Found", "asset not found", head_only);
        return;
    }
    if (send_head(fd, 200, "OK", mime_type(a->path), (long long)a->len, head_only) == 0 &&
        !head_only)
        send_all(fd, (const char *)a->data, a->len);
}

static const char *header_value(const char *raw, const char *name)
{
    const char *p = strstr(raw, "\r\n");
    size_t n = strlen(name);
    if (!p)
        return NULL;
    p += 2;
    while (*p && *p != '\r') {
        const char *e = strstr(p, "\r\n");
        int match = 1;
        if (!e)
            break;
        for (size_t i = 0; i < n; i++) {
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) {
                match = 0;
                break;
            }
        }
        if (match && p[n] == ':') {
            const char *v = p + n + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            return v;
        }
        p = e + 2;
    }
    return NULL;
}

static long long header_content_length(const char *raw)
{
    const char *v = header_value(raw, "Content-Length");
    return v ? atoll(v) : 0;
}

static int drain_body(int fd, const char *raw, size_t raw_len)
{
    const char *hend = strstr(raw, "\r\n\r\n");
    long long cl = header_content_length(raw);
    size_t header_len;
    long long have;
    uint64_t deadline;

    if (!hend || cl <= 0)
        return 1;
    header_len = (size_t)(hend - raw) + 4;
    have = (long long)raw_len - (long long)header_len;
    if (have < 0)
        have = 0;
    if (cl > 512LL * 1024 * 1024)
        return 0;
    deadline = nd_monotonic_ms() + 60000;
    while (have < cl) {
        uint64_t now = nd_monotonic_ms();
        int wait;
        int n;
        char tmp[65536];
        nd_pollfd p;

        if (now >= deadline)
            return 0;
        wait = (int)(deadline - now);
        if (wait > 1000)
            wait = 1000;
        p.fd = fd;
        p.events = ND_POLLIN;
        p.revents = 0;
        if (nd_net_poll(&p, 1, wait) <= 0)
            continue;
        n = nd_net_recv_some(fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
            return 0;
        have += n;
    }
    return 1;
}

static void send_speedtest_empty(int fd, int head_only)
{
    send_head(fd, 200, "OK", "text/plain; charset=utf-8", 0, head_only);
}

static void send_speedtest_ip(int fd, int head_only)
{
    nd_addr peer;
    char ip[96] = "unknown";
    char body[256];
    int n;

    if (nd_net_getpeername(fd, &peer) == 0)
        nd_addr_format(&peer, 0, ip, sizeof(ip));
    n = snprintf(body, sizeof(body),
                 "{\"processedString\":\"%s\",\"rawIspInfo\":\"\"}", ip);
    if (n < 0)
        n = 0;
    if (send_head(fd, 200, "OK", "application/json; charset=utf-8", n, head_only) == 0 &&
        !head_only)
        send_all(fd, body, (size_t)n);
}

static void send_speedtest_download(int fd, const char *target, int head_only)
{
    const char *q = strstr(target, "ckSize=");
    int mb = q ? atoi(q + 7) : 100;
    long long total;
    long long sent = 0;
    char head[512];
    int hlen;
    static unsigned char chunk[65536];
    static int seeded;

    if (mb < 1)
        mb = 1;
    if (mb > 256)
        mb = 256;
    total = (long long)mb * 1024LL * 1024LL;

    if (!seeded) {
        unsigned int x = 0x12345678u;
        for (size_t i = 0; i < sizeof(chunk); i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            chunk[i] = (unsigned char)x;
        }
        seeded = 1;
    }

    hlen = snprintf(head, sizeof(head),
                    "HTTP/1.1 200 OK\r\n"
                    "Server: NetDebug\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Content-Description: File Transfer\r\n"
                    "Content-Length: %lld\r\n"
                    "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    total);
    if (hlen <= 0 || nd_net_send_all(fd, head, (size_t)hlen, 5000) != 0)
        return;
    if (head_only)
        return;

    while (sent < total) {
        size_t n = sizeof(chunk);
        if ((long long)n > total - sent)
            n = (size_t)(total - sent);
        if (nd_net_send_all(fd, chunk, n, 30000) != 0)
            break;
        sent += (long long)n;
    }
}

static void handle_speedtest(int fd, uint64_t service_id, const char *method,
                             const char *path, const char *target,
                             const char *raw, size_t raw_len, int head_only)
{
    int is_get = !strcmp(method, "GET");
    int is_head = !strcmp(method, "HEAD");
    int is_post = !strcmp(method, "POST");

    if (!strcmp(path, "/garbage.php")) {
        if (!is_get && !is_head) {
            send_error(fd, 405, "Method Not Allowed", "GET only", head_only);
            return;
        }
        ND_LOGI("speedtest %llu download", (unsigned long long)service_id);
        send_speedtest_download(fd, target, head_only);
        return;
    }
    if (!strcmp(path, "/empty.php")) {
        if (is_post && !drain_body(fd, raw, raw_len)) {
            send_error(fd, 400, "Bad Request", "upload body timeout", head_only);
            return;
        }
        ND_LOGI("speedtest %llu %s empty", (unsigned long long)service_id, method);
        send_speedtest_empty(fd, head_only);
        return;
    }
    if (!strcmp(path, "/getIP.php")) {
        send_speedtest_ip(fd, head_only);
        return;
    }
    if (!strcmp(path, "/telemetry.php") || !strcmp(path, "/results/telemetry.php")) {
        if (is_post)
            drain_body(fd, raw, raw_len);
        send_head(fd, 200, "OK", "application/json; charset=utf-8", 2, head_only);
        if (!head_only)
            send_all(fd, "{}", 2);
        return;
    }
    if (!strcmp(method, "OPTIONS")) {
        send_head(fd, 204, "No Content", "text/plain; charset=utf-8", 0, 1);
        return;
    }
    {
        char apath[1024];
        speedtest_asset_path(path, apath, sizeof(apath));
        send_web_asset(fd, find_web_asset(apath), head_only);
    }
}

static void handle_request(int fd, uint64_t service_id, const nd_http_srv_cfg *cfg)
{
    char raw[65536];
    char method[16], target[2048], version[16];
    size_t raw_len = 0;
    char path[2048];
    char file[2048];
    int head_only = 0;

    if (!read_request(fd, raw, sizeof(raw), &raw_len)) {
        send_error(fd, 408, "Request Timeout", "request header timeout", 0);
        return;
    }
    if (sscanf(raw, "%15s %2047s %15s", method, target, version) != 3) {
        send_error(fd, 400, "Bad Request", "bad request line", 0);
        return;
    }
    head_only = !strcmp(method, "HEAD");
    if (strcmp(method, "GET") && strcmp(method, "HEAD") &&
        strcmp(method, "POST") && strcmp(method, "PUT") &&
        strcmp(method, "DELETE") && strcmp(method, "OPTIONS")) {
        send_error(fd, 405, "Method Not Allowed", "method not allowed", head_only);
        return;
    }
    if (cfg->echo) {
        send_echo(fd, raw, raw_len, head_only);
        ND_LOGI("http-service %llu %s %s echo", (unsigned long long)service_id, method, target);
        return;
    }
    if (normalize_path(target, path, sizeof(path)) != 0) {
        send_error(fd, 403, "Forbidden", "invalid path", head_only);
        return;
    }
    if (cfg->speedtest) {
        handle_speedtest(fd, service_id, method, path, target, raw, raw_len, head_only);
        return;
    }
    if (!strcmp(method, "OPTIONS")) {
        send_head(fd, 204, "No Content", "text/plain; charset=utf-8", 0, 1);
        return;
    }
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        send_error(fd, 405, "Method Not Allowed", "static server only supports GET/HEAD", head_only);
        return;
    }

    snprintf(file, sizeof(file), "%s%s", cfg->root, path);
    {
        struct stat st;
        if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
            char index[2100];
            struct stat ist;
            snprintf(index, sizeof(index), "%s/index.html", file);
            ND_LOGI("http-service %llu %s %s", (unsigned long long)service_id, method, target);
            if (stat(index, &ist) == 0 && S_ISREG(ist.st_mode))
                serve_file(fd, index, head_only);
            else
                serve_directory(fd, file, path, head_only);
            return;
        }
    }
    ND_LOGI("http-service %llu %s %s", (unsigned long long)service_id, method, target);
    serve_file(fd, file, head_only);
}

static void *http_thread(void *arg)
{
    http_job *job = (http_job *)arg;

    if (job->svc) {
        atomic_fetch_add(&job->svc->http_active, 1);
        atomic_fetch_add(&job->svc->http_requests, 1);
    }
    nd_net_set_nodelay(job->fd);
    handle_request(job->fd, job->service_id, &job->cfg);
    nd_net_close(job->fd);
    if (job->svc) {
        atomic_fetch_sub(&job->svc->http_active, 1);
        nd_service_put(job->svc);
    }
    free(job);
    return NULL;
}

int nd_http_server_start(int fd, nd_service *svc, const nd_http_srv_cfg *cfg)
{
    http_job *job;
    nd_thread th;

    job = (http_job *)calloc(1, sizeof(*job));
    if (!job) {
        if (svc)
            nd_service_put(svc);
        return -1;
    }
    job->fd = fd;
    job->service_id = svc ? svc->id : 0;
    job->svc = svc;
    if (cfg)
        job->cfg = *cfg;
    if (nd_thread_start(&th, http_thread, job, 256 * 1024) != 0) {
        if (svc)
            nd_service_put(svc);
        free(job);
        return -1;
    }
    nd_thread_detach(&th);
    return 0;
}


