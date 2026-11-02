#include "httpd/server.h"
#include "httpd/http.h"
#include "httpd/web_assets.h"
#include "httpd/ws.h"
#include "core/error.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "platform/netio.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fd4 = -1, g_fd6 = -1;
static int g_port = 0;
static char g_token[128];
static nd_thread g_accept_thread;
static atomic_bool g_stopping;

static const char *mime_type(const char *path)
{
    if (nd_ends_with(path, ".html")) return "text/html; charset=utf-8";
    if (nd_ends_with(path, ".js"))   return "application/javascript; charset=utf-8";
    if (nd_ends_with(path, ".css"))  return "text/css; charset=utf-8";
    if (nd_ends_with(path, ".json")) return "application/json; charset=utf-8";
    if (nd_ends_with(path, ".svg"))  return "image/svg+xml";
    if (nd_ends_with(path, ".png"))  return "image/png";
    if (nd_ends_with(path, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static int host_allowed(const char *host)
{
    if (!host || !host[0])
        return 1; /* HTTP/1.0 或本机工具 */
    if (nd_starts_with(host, "127.0.0.1"))
        return 1;
    if (nd_starts_with(host, "localhost"))
        return 1;
    if (nd_starts_with(host, "[::1]"))
        return 1;
    if (nd_starts_with(host, "::1"))
        return 1;
    return 0;
}

static int origin_allowed(const char *origin)
{
    if (!origin || !origin[0])
        return 1;
    if (nd_starts_with(origin, "http://127.0.0.1"))
        return 1;
    if (nd_starts_with(origin, "http://localhost"))
        return 1;
    if (nd_starts_with(origin, "http://[::1]"))
        return 1;
    return 0;
}

static int token_ok(const char *got)
{
    return got && g_token[0] && strcmp(got, g_token) == 0;
}

static const nd_web_asset *asset_find(const char *path)
{
    for (size_t i = 0; i < nd_web_assets_count; i++) {
        if (strcmp(nd_web_assets[i].path, path) == 0)
            return &nd_web_assets[i];
    }
    return NULL;
}

static void serve_static(int fd, const char *path)
{
    const nd_web_asset *a;
    char real[600];

    if (strcmp(path, "/") == 0)
        nd_strlcpy(real, "/index.html", sizeof(real));
    else
        nd_strlcpy(real, path, sizeof(real));

    a = asset_find(real);
    if (!a) {
        nd_http_respond(fd, 404, "text/plain; charset=utf-8", "not found\n", 10);
        return;
    }
    nd_http_respond(fd, 200, mime_type(real), a->data, a->len);
}

static int parse_ws_path(const char *path, uint64_t *conn_id)
{
    if (strcmp(path, "/ws/events") == 0) {
        *conn_id = 0;
        return 1;
    }
    if (nd_starts_with(path, "/ws/conn/")) {
        char *end = NULL;
        unsigned long long v = strtoull(path + 9, &end, 10);
        if (end && *end == '\0') {
            *conn_id = (uint64_t)v;
            return 1;
        }
    }
    return 0;
}

static void handle_client(int fd)
{
    nd_http_req req;
    char err[256] = "";
    const char *host, *origin, *tok;

    nd_net_set_timeouts(fd, 30000, 30000);
    if (nd_http_read_request(fd, &req, err, sizeof(err)) != 0) {
        ND_LOGW("http: %s", err);
        nd_http_respond(fd, 400, "text/plain; charset=utf-8", err, strlen(err));
        nd_net_close(fd);
        return;
    }

    host = nd_http_header(&req, "host");
    if (!host_allowed(host)) {
        nd_http_error_json(fd, 403, ND_E_AUTH_HOST, host);
        nd_net_close(fd);
        nd_http_free_request(&req);
        return;
    }

    if (nd_starts_with(req.path, "/ws/")) {
        uint64_t cid = 0;
        char tbuf[256] = "";
        const char *key = nd_http_header(&req, "sec-websocket-key");
        origin = nd_http_header(&req, "origin");
        if (!parse_ws_path(req.path, &cid) || !token_ok(
                nd_http_query_get(req.query, "t", tbuf, sizeof(tbuf)) == 0 ? tbuf : NULL) ||
            !origin_allowed(origin) || !key) {
            nd_http_error_json(fd, 403, ND_E_AUTH_TOKEN, "ws auth failed");
            nd_net_close(fd);
            nd_http_free_request(&req);
            return;
        }
        nd_http_free_request(&req);
        nd_ws_serve(fd, key, cid); /* 接管 fd */
        return;
    }

    if (nd_starts_with(req.path, "/api/")) {
        origin = nd_http_header(&req, "origin");
        tok = nd_http_header(&req, "x-auth-token");
        if (!token_ok(tok)) {
            nd_http_error_json(fd, 401, ND_E_AUTH_TOKEN, "missing X-Auth-Token");
            nd_net_close(fd);
            nd_http_free_request(&req);
            return;
        }
        if (!origin_allowed(origin)) {
            nd_http_error_json(fd, 403, ND_E_AUTH_ORIGIN, origin);
            nd_net_close(fd);
            nd_http_free_request(&req);
            return;
        }
        if (!nd_api_handle(&req)) {
            nd_http_error_json(fd, 404, ND_E_PARAM, req.path);
        }
        nd_net_close(fd);
        nd_http_free_request(&req);
        return;
    }

    serve_static(fd, req.path);
    nd_net_close(fd);
    nd_http_free_request(&req);
}

static void *client_thread(void *arg)
{
    int fd = *(int *)arg;
    free(arg);
    handle_client(fd);
    return NULL;
}

static void *accept_thread(void *arg)
{
    nd_pollfd fds[2];
    size_t n = 0;
    (void)arg;

    if (g_fd4 >= 0) {
        fds[n].fd = g_fd4;
        fds[n].events = ND_POLLIN;
        fds[n].revents = 0;
        n++;
    }
    if (g_fd6 >= 0) {
        fds[n].fd = g_fd6;
        fds[n].events = ND_POLLIN;
        fds[n].revents = 0;
        n++;
    }

    while (!atomic_load(&g_stopping)) {
        int rc = nd_net_poll(fds, n, 500);
        if (rc <= 0)
            continue;
        for (size_t i = 0; i < n; i++) {
            if (!(fds[i].revents & ND_POLLIN))
                continue;
            for (;;) {
                nd_addr peer, local;
                int cfd = nd_net_accept(fds[i].fd, &peer, &local);
                int *argp;
                nd_thread th;
                if (cfd < 0)
                    break;
                argp = (int *)malloc(sizeof(int));
                if (!argp) {
                    nd_net_close(cfd);
                    continue;
                }
                *argp = cfd;
                if (nd_thread_start(&th, client_thread, argp, 512 * 1024) != 0) {
                    free(argp);
                    nd_net_close(cfd);
                    continue;
                }
                nd_thread_detach(&th);
            }
        }
    }
    return NULL;
}

int nd_http_start(int port, const char *token, int *actual_port, char *err, size_t errlen)
{
    int p;
    int bound4 = 0, bound6 = 0;
    int chosen = 0;

    nd_strlcpy(g_token, token ? token : "", sizeof(g_token));
    atomic_store(&g_stopping, 0);

    for (p = port; p < port + 20; p++) {
        int fd4 = nd_net_socket(AF_INET, SOCK_STREAM);
        int fd6 = -1;
        nd_addr a;
        int ok4 = 0, ok6 = 0;

        if (fd4 >= 0) {
            memset(&a, 0, sizeof(a));
            a.family = AF_INET;
            a.port = (uint16_t)p;
            memcpy(a.raw, "\x7f\0\0\1", 4);
            nd_net_set_reuse(fd4);
            if (nd_net_bind(fd4, &a) == 0 && nd_net_listen(fd4, 64) == 0)
                ok4 = 1;
            else {
                nd_net_close(fd4);
                fd4 = -1;
            }
        }
        if (ok4) {
            fd6 = nd_net_socket(AF_INET6, SOCK_STREAM);
            if (fd6 >= 0) {
                memset(&a, 0, sizeof(a));
                a.family = AF_INET6;
                a.port = (uint16_t)p;
                a.raw[15] = 1; /* ::1 */
                nd_net_set_reuse(fd6);
                nd_net_set_v6only(fd6, 1);
                if (nd_net_bind(fd6, &a) == 0 && nd_net_listen(fd6, 64) == 0)
                    ok6 = 1;
                else {
                    nd_net_close(fd6);
                    fd6 = -1;
                }
            }
        }
        if (ok4) {
            g_fd4 = fd4;
            g_fd6 = fd6;
            bound4 = ok4;
            bound6 = ok6;
            chosen = p;
            nd_net_set_nonblock(fd4, 1);
            if (fd6 >= 0)
                nd_net_set_nonblock(fd6, 1);
            break;
        }
    }

    if (!bound4) {
        if (err)
            snprintf(err, errlen, "无法绑定 127.0.0.1:%d..%d", port, port + 19);
        return -1;
    }

    g_port = chosen;
    if (actual_port)
        *actual_port = chosen;
    ND_LOGI("http: listening on 127.0.0.1:%d%s", chosen, bound6 ? " and [::1]" : "");

    if (nd_thread_start(&g_accept_thread, accept_thread, NULL, 256 * 1024) != 0) {
        if (err)
            snprintf(err, errlen, "无法启动 HTTP 线程");
        return -1;
    }
    return 0;
}

void nd_http_stop(void)
{
    atomic_store(&g_stopping, 1);
    nd_thread_join(&g_accept_thread);
    if (g_fd4 >= 0) {
        nd_net_close(g_fd4);
        g_fd4 = -1;
    }
    if (g_fd6 >= 0) {
        nd_net_close(g_fd6);
        g_fd6 = -1;
    }
}

int nd_http_port(void) { return g_port; }
