#include "httpd/http.h"
#include "core/error.h"
#include "core/log.h"
#include "core/util.h"
#include "platform/netio.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_MAX_HEADER (32 * 1024)
#define HTTP_MAX_BODY   (4 * 1024 * 1024)

static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static int find_header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (int)(i + 4);
    }
    return -1;
}

static void parse_query(nd_http_req *req)
{
    char *q = strchr(req->path, '?');
    if (!q)
        return;
    *q = '\0';
    nd_strlcpy(req->query, q + 1, sizeof(req->query));
}

static int parse_headers(nd_http_req *req, char *buf, size_t len)
{
    char *line = buf;
    char *end = buf + len;

    while (line < end) {
        char *nl = strstr(line, "\r\n");
        if (!nl)
            break;
        *nl = '\0';
        if (nl == line)
            break;
        if (req->n_headers < (int)(sizeof(req->headers) / sizeof(req->headers[0]))) {
            char *colon = strchr(line, ':');
            if (colon) {
                nd_http_hdr *h = &req->headers[req->n_headers++];
                size_t n;
                *colon = '\0';
                nd_strlcpy(h->name, line, sizeof(h->name));
                nd_strlcpy(h->value, colon + 1, sizeof(h->value));
                nd_trim(h->value);
                n = strlen(h->name);
                for (size_t i = 0; i < n; i++)
                    h->name[i] = (char)tolower((unsigned char)h->name[i]);
            }
        }
        line = nl + 2;
    }
    return 0;
}

int nd_http_read_request(int fd, nd_http_req *req, char *err, size_t errlen)
{
    char *buf = NULL;
    size_t cap = HTTP_MAX_HEADER, len = 0;
    int hdr_end = -1;
    long content_length = 0;

    memset(req, 0, sizeof(*req));
    req->fd = fd;
    buf = (char *)malloc(cap);
    if (!buf) {
        if (err) snprintf(err, errlen, "out of memory");
        return -1;
    }

    while (len < cap) {
        int n = nd_net_recv_some(fd, buf + len, cap - len - 1, 10000);
        if (n <= 0) {
            free(buf);
            if (err) snprintf(err, errlen, "连接中断或超时");
            return -1;
        }
        len += (size_t)n;
        buf[len] = '\0';
        hdr_end = find_header_end(buf, len);
        if (hdr_end >= 0)
            break;
    }
    if (hdr_end < 0) {
        free(buf);
        if (err) snprintf(err, errlen, "请求头过大");
        return -1;
    }

    {
        char *first_end = strstr(buf, "\r\n");
        char *sp1, *sp2;
        if (!first_end) {
            free(buf);
            if (err) snprintf(err, errlen, "非法请求行");
            return -1;
        }
        *first_end = '\0';
        sp1 = strchr(buf, ' ');
        if (!sp1) {
            free(buf);
            if (err) snprintf(err, errlen, "非法请求行");
            return -1;
        }
        *sp1 = '\0';
        sp2 = strchr(sp1 + 1, ' ');
        if (sp2)
            *sp2 = '\0';
        nd_strlcpy(req->method, buf, sizeof(req->method));
        nd_strlcpy(req->path, sp1 + 1, sizeof(req->path));
        nd_strlcpy(req->version, sp2 ? sp2 + 1 : "HTTP/1.1", sizeof(req->version));
        parse_query(req);

        parse_headers(req, first_end + 2, len - (size_t)(first_end + 2 - buf));
    }

    {
        const char *cl = nd_http_header(req, "content-length");
        if (cl)
            content_length = atol(cl);
        if (content_length < 0 || content_length > HTTP_MAX_BODY) {
            free(buf);
            if (err) snprintf(err, errlen, "请求体过大");
            return -1;
        }
    }

    {
        size_t body_got = len - (size_t)hdr_end;
        size_t body_want = (size_t)content_length;
        req->body = (char *)malloc(body_want + 1);
        if (!req->body) {
            free(buf);
            return -1;
        }
        if (body_got > body_want)
            body_got = body_want;
        if (body_got)
            memcpy(req->body, buf + hdr_end, body_got);
        req->body_len = body_got;
        while (req->body_len < body_want) {
            int n = nd_net_recv_some(fd, req->body + req->body_len,
                                     body_want - req->body_len, 10000);
            if (n <= 0)
                break;
            req->body_len += (size_t)n;
        }
        req->body[req->body_len] = '\0';
        if (req->body_len != body_want) {
            free(buf);
            if (err) snprintf(err, errlen, "请求体不完整");
            return -1;
        }
    }

    free(buf);
    return 0;
}

void nd_http_free_request(nd_http_req *req)
{
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

const char *nd_http_header(const nd_http_req *req, const char *name)
{
    for (int i = 0; i < req->n_headers; i++) {
#ifdef _WIN32
        if (_stricmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
#else
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
#endif
    }
    return NULL;
}

void nd_http_respond(int fd, int status, const char *ctype, const void *body, size_t len)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Connection: close\r\n\r\n",
                     status, status_text(status), ctype ? ctype : "application/octet-stream", len);
    nd_net_send_all(fd, head, (size_t)n, 10000);
    if (len)
        nd_net_send_all(fd, body, len, 30000);
}

void nd_http_json(int fd, int status, void *cjson_obj)
{
    char *text = cJSON_PrintUnformatted((cJSON *)cjson_obj);
    if (!text) {
        nd_http_respond(fd, 500, "text/plain; charset=utf-8", "json error", 10);
        return;
    }
    nd_http_respond(fd, status, "application/json; charset=utf-8", text, strlen(text));
    free(text);
}

void nd_http_error_json(int fd, int status, int code, const char *detail)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", nd_error_str(code));
    if (detail)
        cJSON_AddStringToObject(o, "detail", detail);
    nd_http_json(fd, status, o);
    cJSON_Delete(o);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void nd_http_url_decode(char *s)
{
    char *p = s;
    while (*s) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *p++ = (char)((hexval(s[1]) << 4) | hexval(s[2]));
            s += 3;
        } else if (*s == '+') {
            *p++ = ' ';
            s++;
        } else {
            *p++ = *s++;
        }
    }
    *p = '\0';
}

int nd_http_query_get(const char *query, const char *name, char *out, size_t cap)
{
    const char *p = query;
    size_t nlen = strlen(name);
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq = strchr(p, '=');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && eq < p + seg && (size_t)(eq - p) == nlen && strncmp(p, name, nlen) == 0) {
            size_t vlen = seg - (size_t)(eq - p) - 1;
            if (vlen >= cap)
                vlen = cap - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            nd_http_url_decode(out);
            return 0;
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    return -1;
}
