#include "net/url.h"
#include "core/error.h"
#include "core/util.h"

#include <stdio.h>
#include <string.h>

static int parse_authority(const char *auth, size_t len, nd_url *out,
                           char *err, size_t errlen)
{
    char tmp[512];
    char *host = tmp;
    char *port = NULL;
    size_t n = len;

    if (n >= sizeof(tmp)) {
        snprintf(err, errlen, "主机名过长");
        return ND_E_PARAM;
    }
    memcpy(tmp, auth, n);
    tmp[n] = '\0';

    if (tmp[0] == '[') {
        char *end = strchr(tmp, ']');
        if (!end) {
            snprintf(err, errlen, "IPv6 地址缺少 ]");
            return ND_E_PARAM;
        }
        *end = '\0';
        host = tmp + 1;
        if (end[1] == ':')
            port = end + 2;
        else if (end[1] != '\0') {
            snprintf(err, errlen, "IPv6 地址后的字符非法");
            return ND_E_PARAM;
        }
    } else {
        char *colon = strchr(tmp, ':');
        if (colon) {
            *colon = '\0';
            port = colon + 1;
        }
    }

    if (!host[0]) {
        snprintf(err, errlen, "主机名为空");
        return ND_E_PARAM;
    }
    if (strlen(host) >= sizeof(out->host)) {
        snprintf(err, errlen, "主机名过长");
        return ND_E_PARAM;
    }
    nd_strlcpy(out->host, host, sizeof(out->host));
    out->port = out->tls ? 443 : 80;
    if (port && port[0]) {
        int v = 0;
        for (char *p = port; *p; p++) {
            if (*p < '0' || *p > '9') {
                snprintf(err, errlen, "端口非法");
                return ND_E_PARAM_PORT;
            }
            v = v * 10 + (*p - '0');
            if (v > 65535) {
                snprintf(err, errlen, "端口超出范围");
                return ND_E_PARAM_PORT;
            }
        }
        if (v <= 0) {
            snprintf(err, errlen, "端口非法");
            return ND_E_PARAM_PORT;
        }
        out->port = (uint16_t)v;
    }
    return ND_OK;
}

int nd_url_parse(const char *text, nd_url *out, char *err, size_t errlen)
{
    const char *p;
    const char *auth;
    size_t auth_len;
    const char *path;

    memset(out, 0, sizeof(*out));
    if (!text || !text[0]) {
        snprintf(err, errlen, "URL 为空");
        return ND_E_PARAM;
    }
    p = strstr(text, "://");
    if (!p) {
        snprintf(err, errlen, "URL 必须以 http:// 或 https:// 开头");
        return ND_E_PARAM;
    }
    if ((size_t)(p - text) >= sizeof(out->scheme)) {
        snprintf(err, errlen, "协议名过长");
        return ND_E_PARAM;
    }
    memcpy(out->scheme, text, (size_t)(p - text));
    out->scheme[p - text] = '\0';
    for (char *s = out->scheme; *s; s++)
        *s = (char)((*s >= 'A' && *s <= 'Z') ? *s - 'A' + 'a' : *s);

    if (!strcmp(out->scheme, "https"))
        out->tls = 1;
    else if (strcmp(out->scheme, "http") != 0) {
        snprintf(err, errlen, "不支持的协议: %s", out->scheme);
        return ND_E_PARAM;
    }

    auth = p + 3;
    if (strchr(auth, '@')) {
        snprintf(err, errlen, "暂不支持 URL 中的用户信息");
        return ND_E_PARAM;
    }
    path = strpbrk(auth, "/?#");
    auth_len = path ? (size_t)(path - auth) : strlen(auth);

    {
        int rc = parse_authority(auth, auth_len, out, err, errlen);
        if (rc != ND_OK)
            return rc;
    }

    if (!path || *path == '\0' || *path == '#') {
       nd_strlcpy(out->path, "/", sizeof(out->path));
    } else if (*path == '?') {
        snprintf(out->path, sizeof(out->path), "/%s", path);
    } else {
        size_t len = strcspn(path, "#");
        if (len >= sizeof(out->path)) {
            snprintf(err, errlen, "路径过长");
            return ND_E_PARAM;
        }
        memcpy(out->path, path, len);
        out->path[len] = '\0';
    }
    return ND_OK;
}

int nd_url_resolve(const nd_url *base, const char *location, nd_url *out,
                   char *err, size_t errlen)
{
    char buf[4096];
    int def_port = base->tls ? 443 : 80;
    int bracketed = strchr(base->host, ':') != NULL;

    if (!location || !location[0]) {
        snprintf(err, errlen, "Location 为空");
        return ND_E_PARAM;
    }
    if (strstr(location, "://"))
        return nd_url_parse(location, out, err, errlen);

    if (location[0] == '/') {
        if (bracketed) {
            if (base->port == def_port)
                snprintf(buf, sizeof(buf), "%s://[%s]%s", base->scheme, base->host, location);
            else
                snprintf(buf, sizeof(buf), "%s://[%s]:%u%s", base->scheme, base->host,
                         (unsigned)base->port, location);
        } else {
            if (base->port == def_port)
                snprintf(buf, sizeof(buf), "%s://%s%s", base->scheme, base->host, location);
            else
                snprintf(buf, sizeof(buf), "%s://%s:%u%s", base->scheme, base->host,
                         (unsigned)base->port, location);
        }
        return nd_url_parse(buf, out, err, errlen);
    }

    /* 相对路径：拼到当前目录后 */
    {
        char dir[2048];
        size_t i = strlen(base->path);
        while (i > 0 && base->path[i - 1] != '/')
            i--;
        if (i == 0)
            i = 1;
        memcpy(dir, base->path, i);
        dir[i] = '\0';
        if (bracketed) {
            if (base->port == def_port)
                snprintf(buf, sizeof(buf), "%s://[%s]%s%s", base->scheme, base->host, dir, location);
            else
                snprintf(buf, sizeof(buf), "%s://[%s]:%u%s%s", base->scheme, base->host,
                         (unsigned)base->port, dir, location);
        } else {
            if (base->port == def_port)
                snprintf(buf, sizeof(buf), "%s://%s%s%s", base->scheme, base->host, dir, location);
            else
                snprintf(buf, sizeof(buf), "%s://%s:%u%s%s", base->scheme, base->host,
                         (unsigned)base->port, dir, location);
        }
    }
    return nd_url_parse(buf, out, err, errlen);
}
