#include "httpd/api.h"
#include "httpd/server.h"
#include "httpd/http.h"
#include "httpd/ws.h"
#include "httpd/base64.h"
#include "core/error.h"
#include "core/log.h"
#include "core/util.h"
#include "net/bind_json.h"
#include "net/iface.h"
#include "platform/netio.h"
#include "platform/platform.h"
#include "platform/serial.h"
#include "proto/ftp_client.h"
#include "proto/http_client.h"
#include "proto/tftp_client.h"
#include "proto/smb2_client.h"
#include "session/conn.h"
#include "session/smb.h"
#include "session/ftp.h"
#include "session/service.h"
#include "session/session.h"
#include "session/store.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int path_id(const char *path, const char *prefix, uint64_t *id, const char **rest);

static const char *json_str(cJSON *o, const char *key, const char *def){
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : def;
}

static int json_int(cJSON *o, const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : def;
}

static void add_slots(cJSON *o, const nd_plan *p)
{
    cJSON *arr = cJSON_AddArrayToObject(o, "slots");
    for (size_t i = 0; i < p->n; i++) {
        char a[96];
        char hint[256];
        cJSON *s = cJSON_CreateObject();
        nd_addr_format(&p->slot[i].local, 1, a, sizeof(a));
        if (p->slot[i].sys_errno) {
            char se[128];
            nd_sys_error(p->slot[i].sys_errno, se, sizeof(se));
            snprintf(hint, sizeof(hint), "%s：%s", nd_error_str(p->slot[i].err), se);
        } else {
            snprintf(hint, sizeof(hint), "%s", nd_error_str(p->slot[i].err));
        }
        cJSON_AddStringToObject(s, "addr", a);
        cJSON_AddBoolToObject(s, "ok", p->slot[i].fd >= 0);
        cJSON_AddNumberToObject(s, "code", p->slot[i].err);
        cJSON_AddNumberToObject(s, "sys_errno", p->slot[i].sys_errno);
        if (p->slot[i].err)
            cJSON_AddStringToObject(s, "hint", hint);
        cJSON_AddItemToArray(arr, s);
    }
    cJSON_AddBoolToObject(o, "degraded", p->degraded);
    cJSON_AddBoolToObject(o, "fatal", p->fatal);
}

static cJSON *service_json(nd_service *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)s->id);
    cJSON_AddStringToObject(o, "name", s->name);
    cJSON_AddStringToObject(o, "proto", s->proto);
    cJSON_AddItemToObject(o, "bind", nd_bind_to_json(&s->bind));
    cJSON_AddBoolToObject(o, "running", nd_service_running(s));
    if (!strcmp(s->proto, "http") || !strcmp(s->proto, "speedtest"))
        cJSON_AddNumberToObject(o, "conns", (double)atomic_load(&s->http_active));
    else
        cJSON_AddNumberToObject(o, "conns", (double)nd_service_conn_count(s));
    cJSON_AddNumberToObject(o, "requests", (double)atomic_load(&s->http_requests));
    cJSON_AddNumberToObject(o, "peak", (double)atomic_load(&s->peak_conns));
    {
        uint64_t started = atomic_load(&s->started_ms);
        uint64_t up = (started && nd_service_running(s)) ? (nd_monotonic_ms() - started) : 0;
        cJSON_AddNumberToObject(o, "uptime_ms", (double)up);
    }
    cJSON_AddBoolToObject(o, "echo", s->echo);
    if (!strcmp(s->proto, "ftp")) {
        cJSON *op = cJSON_CreateObject();
        cJSON_AddStringToObject(op, "root", s->ftp_root);
        cJSON_AddBoolToObject(op, "readonly", s->ftp_readonly);
        cJSON_AddBoolToObject(op, "anonymous", s->ftp_anonymous);
        cJSON_AddStringToObject(op, "user", s->ftp_user);
        cJSON_AddStringToObject(op, "advertise", s->ftp_advertise);
        {
            cJSON *pp = cJSON_CreateArray();
            cJSON_AddItemToArray(pp, cJSON_CreateNumber(s->pasv_min));
            cJSON_AddItemToArray(pp, cJSON_CreateNumber(s->pasv_max));
            cJSON_AddItemToObject(op, "pasv_ports", pp);
        }
        cJSON_AddItemToObject(o, "options", op);
    }
    if (!strcmp(s->proto, "tftp")) {
        cJSON *op = cJSON_CreateObject();
        cJSON_AddStringToObject(op, "root", s->tftp_root);
        cJSON_AddBoolToObject(op, "readonly", s->tftp_readonly);
        cJSON_AddNumberToObject(op, "timeout", s->tftp_timeout);
        cJSON_AddItemToObject(o, "options", op);
    }
    if (!strcmp(s->proto, "http")) {
        cJSON *op = cJSON_CreateObject();
        cJSON_AddStringToObject(op, "root", s->http_root);
        cJSON_AddBoolToObject(op, "echo", s->echo);
        cJSON_AddItemToObject(o, "options", op);
    }
    if (s->last_error[0])
        cJSON_AddStringToObject(o, "last_error", s->last_error);
    if (s->plan.n)
        add_slots(o, &s->plan);
    return o;
}

static cJSON *conn_json(nd_conn *c)
{
    char peer[96] = "-", local[96] = "-";
    cJSON *o = cJSON_CreateObject();
    if (c->peer.family)
        nd_addr_format(&c->peer, 1, peer, sizeof(peer));
    if (c->local.family)
        nd_addr_format(&c->local, 1, local, sizeof(local));
    cJSON_AddNumberToObject(o, "id", (double)c->id);
    cJSON_AddNumberToObject(o, "service_id", (double)c->service_id);
    cJSON_AddStringToObject(o, "proto", c->proto);
    cJSON_AddStringToObject(o, "peer", peer);
    cJSON_AddStringToObject(o, "local", local);
    cJSON_AddNumberToObject(o, "rx", (double)c->rx_bytes);
    cJSON_AddNumberToObject(o, "tx", (double)c->tx_bytes);
    cJSON_AddNumberToObject(o, "uptime_ms",
                            (double)(nd_monotonic_ms() - c->started_ms));
    cJSON_AddBoolToObject(o, "closed", nd_conn_is_closed(c));
    return o;
}

static const char *session_state_str(int st)
{
    switch (st) {
    case ND_SESS_IDLE:       return "idle";
    case ND_SESS_CONNECTING: return "connecting";
    case ND_SESS_OPEN:       return "open";
    case ND_SESS_ERROR:      return "error";
    default:                 return "?";
    }
}

static cJSON *session_json(nd_session *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)s->id);
    cJSON_AddStringToObject(o, "name", s->name);
    cJSON_AddStringToObject(o, "host", s->host);
    cJSON_AddNumberToObject(o, "port", s->port);
    cJSON_AddStringToObject(o, "family", nd_family_name(s->family));
    cJSON_AddStringToObject(o, "transport",
                            s->ssh ? "ssh" :
                            (s->serial ? "serial" : (s->transport ? "udp" : "tcp")));
    cJSON_AddStringToObject(o, "protocol",
                            s->ssh ? "ssh" :
                            (s->serial ? "serial" : (s->telnet ? "telnet" : "raw")));
    if (s->ssh) {
        cJSON_AddStringToObject(o, "user", s->ssh_user);
        cJSON_AddStringToObject(o, "fingerprint", s->ssh_fp);
    }
    if (s->serial) {
        cJSON_AddStringToObject(o, "serial_port", s->serial_port);
        cJSON_AddNumberToObject(o, "baud", s->baud);
        cJSON_AddNumberToObject(o, "data_bits", s->data_bits);
        cJSON_AddNumberToObject(o, "parity", s->parity);
        cJSON_AddNumberToObject(o, "stop_bits", s->stop_bits);
        cJSON_AddNumberToObject(o, "flow", s->flow);
    }
    cJSON_AddStringToObject(o, "iface_id", s->iface_id);
    cJSON_AddStringToObject(o, "src_addr", s->src_addr);
    cJSON_AddStringToObject(o, "state", session_state_str(nd_session_get_state(s)));
    cJSON_AddNumberToObject(o, "conn_id", (double)nd_session_get_conn(s));
    if (s->last_error[0])
        cJSON_AddStringToObject(o, "last_error", s->last_error);
    return o;
}

static cJSON *http_result_json(nd_http_result *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *hs;
    cJSON *tm;

    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddNumberToObject(o, "status", r->status);
    cJSON_AddStringToObject(o, "reason", r->reason);
    cJSON_AddStringToObject(o, "version", r->version);
    cJSON_AddStringToObject(o, "final_url", r->final_url);
    cJSON_AddNumberToObject(o, "redirects", r->redirects);
    cJSON_AddNumberToObject(o, "body_len", (double)r->body_len);
    cJSON_AddBoolToObject(o, "truncated", r->truncated);
    if (r->content_encoding[0])
        cJSON_AddStringToObject(o, "content_encoding", r->content_encoding);

    hs = cJSON_AddArrayToObject(o, "headers");
    for (int i = 0; i < r->n_headers; i++) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "name", r->headers[i].name);
        cJSON_AddStringToObject(h, "value", r->headers[i].value);
        cJSON_AddItemToArray(hs, h);
    }

    tm = cJSON_CreateObject();
    cJSON_AddNumberToObject(tm, "connect_ms", (double)r->connect_ms);
    cJSON_AddNumberToObject(tm, "first_byte_ms", (double)r->first_byte_ms);
    cJSON_AddNumberToObject(tm, "total_ms", (double)r->total_ms);
    cJSON_AddItemToObject(o, "timings", tm);

    if (r->req_head)
        cJSON_AddStringToObject(o, "request_head", r->req_head);
    if (r->resp_head)
        cJSON_AddStringToObject(o, "response_head", r->resp_head);

    if (r->body && r->body_len) {
        int is_text = !memchr(r->body, 0, r->body_len) &&
                      nd_utf8_valid(r->body, r->body_len);
        if (is_text) {
            char *t = (char *)malloc(r->body_len + 1);
            if (t) {
                memcpy(t, r->body, r->body_len);
                t[r->body_len] = '\0';
                cJSON_AddStringToObject(o, "body_text", t);
                free(t);
            }
        } else {
            size_t cap = ((r->body_len + 2) / 3) * 4 + 1;
            char *b = (char *)malloc(cap);
            if (b && nd_base64_encode(r->body, r->body_len, b, cap) >= 0)
                cJSON_AddStringToObject(o, "body_base64", b);
            free(b);
        }
    }
    return o;
}

static const char *path_base(const char *p)
{
    const char *a = strrchr(p, '/');
    const char *b = strrchr(p, '\\');
    if (b > a)
        a = b;
    return a ? a + 1 : p;
}

static cJSON *ftp_sess_json(nd_ftp_sess *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)s->id);
    cJSON_AddStringToObject(o, "host", s->host);
    cJSON_AddNumberToObject(o, "port", s->port);
    cJSON_AddStringToObject(o, "user", s->user);
    cJSON_AddStringToObject(o, "family", nd_family_name(s->family));
    cJSON_AddStringToObject(o, "pwd", s->pwd);
    return o;
}

static int ftp_open_session(nd_http_req *req)
{
    cJSON *jb = cJSON_Parse(req->body ? req->body : "");
    const char *host;
    const char *user;
    const char *pass;
    const char *fams;
    int port;
    nd_family family = ND_FAM_DUAL;
    char err[512] = "";
    nd_ftp *client;
    nd_ftp_sess *s;

    if (!jb) {
        nd_http_error_json(req->fd, 400, ND_E_PARAM, "body 不是合法 JSON");
        return 1;
    }
    host = json_str(jb, "host", "");
    user = json_str(jb, "user", "anonymous");
    pass = json_str(jb, "password", "");
    fams = json_str(jb, "family", "dual");
    port = json_int(jb, "port", 21);
    if (!host[0] || port <= 0 || port > 65535) {
        cJSON_Delete(jb);
        nd_http_error_json(req->fd, 400, ND_E_PARAM, "host/port 不合法");
        return 1;
    }
    if (!strcmp(fams, "v4"))
        family = ND_FAM_V4;
    else if (!strcmp(fams, "v6"))
        family = ND_FAM_V6;

    client = nd_ftp_connect(host, (uint16_t)port, family, 8000, err, sizeof(err));
    if (!client) {
        cJSON *o = cJSON_CreateObject();
        cJSON_Delete(jb);
        cJSON_AddBoolToObject(o, "ok", 0);
        cJSON_AddNumberToObject(o, "code", ND_E_CONNECT_ALL);
        cJSON_AddStringToObject(o, "message", "FTP 连接失败");
        cJSON_AddStringToObject(o, "detail", err);
        nd_http_json(req->fd, 502, o);
        cJSON_Delete(o);
        return 1;
    }
    if (nd_ftp_login(client, user, pass, err, sizeof(err)) != ND_OK) {
        cJSON *o = cJSON_CreateObject();
        nd_ftp_close(client);
        cJSON_Delete(jb);
        cJSON_AddBoolToObject(o, "ok", 0);
        cJSON_AddNumberToObject(o, "code", ND_E_PROTO_AUTH);
        cJSON_AddStringToObject(o, "message", "FTP 登录失败");
        cJSON_AddStringToObject(o, "detail", err);
        nd_http_json(req->fd, 502, o);
        cJSON_Delete(o);
        return 1;
    }
    s = nd_ftp_sess_new(host, (uint16_t)port, user, family, client);
    cJSON_Delete(jb);
    if (!s) {
        nd_ftp_close(client);
        nd_http_error_json(req->fd, 500, ND_E_INTERNAL, "创建会话失败");
        return 1;
    }
    if (nd_ftp_pwd(client, s->pwd, sizeof(s->pwd), err, sizeof(err)) != ND_OK)
        nd_strlcpy(s->pwd, "/", sizeof(s->pwd));
    {
        cJSON *o = ftp_sess_json(s);
        nd_http_json(req->fd, 201, o);
        cJSON_Delete(o);
    }
    return 1;
}

static int ftp_simple_op(nd_http_req *req, nd_ftp_sess *s, const char *op,
                         const char *path)
{
    char err[512] = "";
    int rc = ND_E_PARAM;

    if (!strcmp(op, "cwd"))
        rc = nd_ftp_cwd(s->client, path, err, sizeof(err));
    else if (!strcmp(op, "mkdir"))
        rc = nd_ftp_mkdir(s->client, path, err, sizeof(err));
    else if (!strcmp(op, "rmdir"))
        rc = nd_ftp_rmdir(s->client, path, err, sizeof(err));
    else if (!strcmp(op, "delete"))
        rc = nd_ftp_delete(s->client, path, err, sizeof(err));

    if (rc != ND_OK) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 0);
        cJSON_AddNumberToObject(o, "code", rc);
        cJSON_AddStringToObject(o, "message", nd_error_str(rc));
        cJSON_AddStringToObject(o, "detail", err);
        nd_http_json(req->fd, 502, o);
        cJSON_Delete(o);
        return 1;
    }
    if (!strcmp(op, "cwd"))
        nd_ftp_pwd(s->client, s->pwd, sizeof(s->pwd), err, sizeof(err));
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddStringToObject(o, "pwd", s->pwd);
        nd_http_json(req->fd, 200, o);
        cJSON_Delete(o);
    }
    return 1;
}

static int ftp_handle(nd_http_req *req)
{
    uint64_t id = 0;
    const char *rest = NULL;
    nd_ftp_sess *s;

    if (!strcmp(req->path, "/api/ftp")) {
        if (!strcmp(req->method, "POST"))
            return ftp_open_session(req);
        if (!strcmp(req->method, "GET")) {
            uint64_t ids[64];
            size_t n = nd_ftp_sess_list(ids, 64);
            cJSON *arr = cJSON_CreateArray();
            for (size_t i = 0; i < n; i++) {
                nd_ftp_sess *x = nd_ftp_sess_get(ids[i]);
                if (x) {
                    cJSON_AddItemToArray(arr, ftp_sess_json(x));
                    nd_ftp_sess_put(x);
                }
            }
            nd_http_json(req->fd, 200, arr);
            cJSON_Delete(arr);
            return 1;
        }
    }

    if (path_id(req->path, "/api/ftp/", &id, &rest) != 0)
        return 0;
    s = nd_ftp_sess_get(id);
    if (!s) {
        nd_http_error_json(req->fd, 404, ND_E_PARAM, "FTP 会话不存在");
        return 1;
    }

    if (!strcmp(rest, "/list") && !strcmp(req->method, "GET")) {
        char path[1024] = "";
        char err[512] = "";
        nd_ftp_entry *entries = NULL;
        size_t n = 0;
        cJSON *arr;
        int rc;

        nd_http_query_get(req->query, "path", path, sizeof(path));
        rc = nd_ftp_list(s->client, path, &entries, &n, err, sizeof(err));
        if (rc != ND_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 0);
            cJSON_AddNumberToObject(o, "code", rc);
            cJSON_AddStringToObject(o, "detail", err);
            nd_http_json(req->fd, 502, o);
            cJSON_Delete(o);
            nd_ftp_sess_put(s);
            return 1;
        }
        arr = cJSON_CreateArray();
        for (size_t i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "name", entries[i].name);
            cJSON_AddBoolToObject(e, "is_dir", entries[i].is_dir);
            cJSON_AddNumberToObject(e, "size", (double)entries[i].size);
            cJSON_AddStringToObject(e, "perms", entries[i].perms);
            cJSON_AddStringToObject(e, "modify", entries[i].modify);
            cJSON_AddItemToArray(arr, e);
        }
        free(entries);
        nd_http_json(req->fd, 200, arr);
        cJSON_Delete(arr);
        nd_ftp_sess_put(s);
        return 1;
    }

    if (!strcmp(rest, "/cwd") || !strcmp(rest, "/mkdir") ||
        !strcmp(rest, "/rmdir") || !strcmp(rest, "/delete")) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *path = json_str(jb, "path", "");
        int r = ftp_simple_op(req, s, rest + 1, path);
        cJSON_Delete(jb);
        nd_ftp_sess_put(s);
        return r;
    }

    if (!strcmp(rest, "/rename") && !strcmp(req->method, "POST")) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *from = json_str(jb, "from", "");
        const char *to = json_str(jb, "to", "");
        char err[512] = "";
        int rc = nd_ftp_rename(s->client, from, to, err, sizeof(err));
        cJSON_Delete(jb);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", rc == ND_OK);
            if (rc != ND_OK) {
                cJSON_AddNumberToObject(o, "code", rc);
                cJSON_AddStringToObject(o, "detail", err);
            }
            nd_http_json(req->fd, rc == ND_OK ? 200 : 502, o);
            cJSON_Delete(o);
        }
        nd_ftp_sess_put(s);
        return 1;
    }

    if (!strcmp(rest, "/download") && !strcmp(req->method, "POST")) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *remote = json_str(jb, "remote", "");
        char dir[600], local[900];
        char err[512] = "";
        uint64_t bytes = 0;
        int rc;

        snprintf(dir, sizeof(dir), "%sdownloads", nd_config_dir());
        nd_mkdir_p(dir);
        snprintf(local, sizeof(local), "%s/ftp-%llu-%s", dir,
                 (unsigned long long)s->id, path_base(remote));
        rc = nd_ftp_download(s->client, remote, local, &bytes, err, sizeof(err));
        cJSON_Delete(jb);
        if (rc != ND_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 0);
            cJSON_AddNumberToObject(o, "code", rc);
            cJSON_AddStringToObject(o, "detail", err);
            nd_http_json(req->fd, 502, o);
            cJSON_Delete(o);
        } else {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
            cJSON_AddStringToObject(o, "local_path", local);
            cJSON_AddStringToObject(o, "name", path_base(remote));
            cJSON_AddNumberToObject(o, "size", (double)bytes);
            nd_http_json(req->fd, 200, o);
            cJSON_Delete(o);
        }
        nd_ftp_sess_put(s);
        return 1;
    }

    if (!strcmp(rest, "/upload") && !strcmp(req->method, "POST")) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *remote = json_str(jb, "remote", "");
        const char *b64 = json_str(jb, "data_base64", "");
        size_t cap = (strlen(b64) * 3) / 4 + 4;
        unsigned char *data = (unsigned char *)malloc(cap);
        int n = data ? nd_base64_decode(b64, data, cap) : -1;
        char dir[600], local[900];
        char err[512] = "";
        uint64_t bytes = 0;
        int rc = ND_E_PARAM;

        if (n < 0) {
            free(data);
            cJSON_Delete(jb);
            nd_http_error_json(req->fd, 400, ND_E_PARAM, "data_base64 非法");
            nd_ftp_sess_put(s);
            return 1;
        }
        snprintf(dir, sizeof(dir), "%suploads", nd_config_dir());
        nd_mkdir_p(dir);
        snprintf(local, sizeof(local), "%s/ftp-%llu-%s", dir,
                 (unsigned long long)s->id, path_base(remote));
        if (nd_write_file(local, data, (size_t)n) == 0)
            rc = nd_ftp_upload(s->client, local, remote, &bytes, err, sizeof(err));
        free(data);
        cJSON_Delete(jb);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", rc == ND_OK);
            if (rc == ND_OK)
                cJSON_AddNumberToObject(o, "size", (double)bytes);
            else {
                cJSON_AddNumberToObject(o, "code", rc);
                cJSON_AddStringToObject(o, "detail", err);
            }
            nd_http_json(req->fd, rc == ND_OK ? 200 : 502, o);
            cJSON_Delete(o);
        }
        nd_ftp_sess_put(s);
        return 1;
    }

    if (!strcmp(rest, "") && !strcmp(req->method, "DELETE")) {
        nd_ftp_sess_free(s);
        nd_ftp_sess_put(s);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
            nd_http_json(req->fd, 200, o);
            cJSON_Delete(o);
        }
        return 1;
    }
    if (!strcmp(rest, "") && !strcmp(req->method, "GET")) {
        cJSON *o = ftp_sess_json(s);
        nd_http_json(req->fd, 200, o);
        cJSON_Delete(o);
        nd_ftp_sess_put(s);
        return 1;
    }

    nd_ftp_sess_put(s);
    return 0;
}

static int fs_download(nd_http_req *req)
{
    char path[1024] = "";
    char prefix[600];
    void *data = NULL;
    size_t len = 0;

    nd_http_query_get(req->query, "path", path, sizeof(path));
    snprintf(prefix, sizeof(prefix), "%sdownloads", nd_config_dir());
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        snprintf(prefix, sizeof(prefix), "%suploads", nd_config_dir());
        if (strncmp(path, prefix, strlen(prefix)) != 0) {
            nd_http_error_json(req->fd, 403, ND_E_AUTH, "只允许下载本工具生成的文件");
            return 1;
        }
    }
    if (strstr(path, "..")) {
        nd_http_error_json(req->fd, 403, ND_E_AUTH, "路径非法");
        return 1;
    }
    if (nd_read_file(path, &data, &len) != 0) {
        nd_http_error_json(req->fd, 404, ND_E_IO_DISK, "文件不存在");
        return 1;
    }
    nd_http_respond(req->fd, 200, "application/octet-stream", data, len);
    free(data);
    return 1;
}

static int tftp_api(nd_http_req *req)
{
    cJSON *jb = cJSON_Parse(req->body ? req->body : "");
    const char *host;
    const char *remote;
    const char *fams;
    int port;
    nd_family family = ND_FAM_DUAL;
    int blksize;
    int timeout_s;
    char err[512] = "";
    uint64_t bytes = 0;
    int rc;
    int is_get = strstr(req->path, "/download") != NULL;

    if (!jb) {
        nd_http_error_json(req->fd, 400, ND_E_PARAM, "body 不是合法 JSON");
        return 1;
    }
    host = json_str(jb, "host", "");
    remote = json_str(jb, "remote", "");
    fams = json_str(jb, "family", "dual");
    port = json_int(jb, "port", 69);
    blksize = json_int(jb, "blksize", 1468);
    timeout_s = json_int(jb, "timeout", 3);
    if (!host[0] || !remote[0] || port <= 0 || port > 65535) {
        cJSON_Delete(jb);
        nd_http_error_json(req->fd, 400, ND_E_PARAM, "host/port/remote 不合法");
        return 1;
    }
    if (!strcmp(fams, "v4"))
        family = ND_FAM_V4;
    else if (!strcmp(fams, "v6"))
        family = ND_FAM_V6;

    if (is_get) {
        char dir[600], local[900];
        snprintf(dir, sizeof(dir), "%sdownloads", nd_config_dir());
        nd_mkdir_p(dir);
        snprintf(local, sizeof(local), "%s/tftp-%llu-%s", dir,
                 (unsigned long long)nd_monotonic_ms(), path_base(remote));
        rc = nd_tftp_download(host, (uint16_t)port, family, remote, local,
                              blksize, timeout_s, &bytes, err, sizeof(err));
        cJSON_Delete(jb);
        if (rc != ND_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 0);
            cJSON_AddNumberToObject(o, "code", rc);
            cJSON_AddStringToObject(o, "detail", err);
            nd_http_json(req->fd, 502, o);
            cJSON_Delete(o);
        } else {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
            cJSON_AddStringToObject(o, "local_path", local);
            cJSON_AddStringToObject(o, "name", path_base(remote));
            cJSON_AddNumberToObject(o, "size", (double)bytes);
            nd_http_json(req->fd, 200, o);
            cJSON_Delete(o);
        }
        return 1;
    }

    {
        const char *b64 = json_str(jb, "data_base64", "");
        size_t cap = (strlen(b64) * 3) / 4 + 4;
        unsigned char *data = (unsigned char *)malloc(cap);
        int n = data ? nd_base64_decode(b64, data, cap) : -1;
        char dir[600], local[900];

        if (n < 0) {
            free(data);
            cJSON_Delete(jb);
            nd_http_error_json(req->fd, 400, ND_E_PARAM, "data_base64 非法");
            return 1;
        }
        snprintf(dir, sizeof(dir), "%suploads", nd_config_dir());
        nd_mkdir_p(dir);
        snprintf(local, sizeof(local), "%s/tftp-%llu-%s", dir,
                 (unsigned long long)nd_monotonic_ms(), path_base(remote));
        if (nd_write_file(local, data, (size_t)n) == 0)
            rc = nd_tftp_upload(host, (uint16_t)port, family, local, remote,
                                blksize, timeout_s, &bytes, err, sizeof(err));
        else
            rc = ND_E_IO_DISK;
        free(data);
        cJSON_Delete(jb);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", rc == ND_OK);
            if (rc == ND_OK)
                cJSON_AddNumberToObject(o, "size", (double)bytes);
            else {
                cJSON_AddNumberToObject(o, "code", rc);
                cJSON_AddStringToObject(o, "detail", err);
            }
            nd_http_json(req->fd, rc == ND_OK ? 200 : 502, o);
            cJSON_Delete(o);
        }
    }
    return 1;
}

static cJSON *smb_sess_json(nd_smb_sess *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)s->id);
    cJSON_AddStringToObject(o, "host", s->host);
    cJSON_AddStringToObject(o, "share", s->share);
    cJSON_AddStringToObject(o, "user", s->user);
    cJSON_AddStringToObject(o, "domain", s->domain);
    return o;
}

static int smb_api(nd_http_req *req)
{
    uint64_t id = 0;
    const char *rest = NULL;
    nd_smb_sess *s;

    if (!strcmp(req->path, "/api/smb")) {
        if (!strcmp(req->method, "POST")) {
            cJSON *jb = cJSON_Parse(req->body ? req->body : "");
            char err[512] = "";
            nd_smb2 *client;
            nd_smb_sess *ns;
            if (!jb) {
                nd_http_error_json(req->fd, 400, ND_E_PARAM, "body 不是合法 JSON");
                return 1;
            }
            client = nd_smb2_connect(json_str(jb, "host", ""),
                                     json_str(jb, "share", ""),
                                     json_str(jb, "user", ""),
                                     json_str(jb, "password", ""),
                                     json_str(jb, "domain", ""),
                                     err, sizeof(err));
            if (!client) {
                cJSON *o = cJSON_CreateObject();
                cJSON_Delete(jb);
                cJSON_AddBoolToObject(o, "ok", 0);
                cJSON_AddStringToObject(o, "detail", err);
                nd_http_json(req->fd, 502, o);
                cJSON_Delete(o);
                return 1;
            }
            ns = nd_smb_sess_new(json_str(jb, "host", ""), json_str(jb, "share", ""),
                                 json_str(jb, "user", ""), json_str(jb, "domain", ""), client);
            cJSON_Delete(jb);
            if (!ns) {
                nd_smb2_close(client);
                nd_http_error_json(req->fd, 500, ND_E_INTERNAL, "创建 SMB 会话失败");
                return 1;
            }
            {
                cJSON *o = smb_sess_json(ns);
                nd_http_json(req->fd, 201, o);
                cJSON_Delete(o);
            }
            return 1;
        }
        if (!strcmp(req->method, "GET")) {
            uint64_t ids[64];
            size_t n = nd_smb_sess_list(ids, 64);
            cJSON *arr = cJSON_CreateArray();
            for (size_t i = 0; i < n; i++) {
                nd_smb_sess *x = nd_smb_sess_get(ids[i]);
                if (x) {
                    cJSON_AddItemToArray(arr, smb_sess_json(x));
                    nd_smb_sess_put(x);
                }
            }
            nd_http_json(req->fd, 200, arr);
            cJSON_Delete(arr);
            return 1;
        }
    }

    if (path_id(req->path, "/api/smb/", &id, &rest) != 0)
        return 0;
    s = nd_smb_sess_get(id);
    if (!s) {
        nd_http_error_json(req->fd, 404, ND_E_PARAM, "SMB 会话不存在");
        return 1;
    }

    if (!strcmp(rest, "/list") && !strcmp(req->method, "GET")) {
        char path[1024] = "";
        char err[512] = "";
        nd_smb_entry *ents = NULL;
        size_t n = 0;
        int rc;
        nd_http_query_get(req->query, "path", path, sizeof(path));
        rc = nd_smb2_list(s->client, path, &ents, &n, err, sizeof(err));
        if (rc != ND_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 0);
            cJSON_AddStringToObject(o, "detail", err);
            nd_http_json(req->fd, 502, o);
            cJSON_Delete(o);
        } else {
            cJSON *arr = cJSON_CreateArray();
            for (size_t i = 0; i < n; i++) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "name", ents[i].name);
                cJSON_AddBoolToObject(e, "is_dir", ents[i].is_dir);
                cJSON_AddNumberToObject(e, "size", (double)ents[i].size);
                cJSON_AddNumberToObject(e, "mtime", (double)ents[i].mtime);
                cJSON_AddItemToArray(arr, e);
            }
            free(ents);
            nd_http_json(req->fd, 200, arr);
            cJSON_Delete(arr);
        }
        nd_smb_sess_put(s);
        return 1;
    }

    if ((!strcmp(rest, "/mkdir") || !strcmp(rest, "/delete") || !strcmp(rest, "/rmdir")) &&
        !strcmp(req->method, "POST")) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *path = json_str(jb, "path", "");
        char err[512] = "";
        int rc = ND_E_PARAM;
        if (!strcmp(rest, "/mkdir"))
            rc = nd_smb2_mkdir(s->client, path, err, sizeof(err));
        else if (!strcmp(rest, "/delete"))
            rc = nd_smb2_unlink(s->client, path, err, sizeof(err));
        else
            rc = nd_smb2_rmdir(s->client, path, err, sizeof(err));
        cJSON_Delete(jb);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", rc == ND_OK);
            if (rc != ND_OK)
                cJSON_AddStringToObject(o, "detail", err);
            nd_http_json(req->fd, rc == ND_OK ? 200 : 502, o);
            cJSON_Delete(o);
        }
        nd_smb_sess_put(s);
        return 1;
    }

    if (!strcmp(rest, "/download") && !strcmp(req->method, "POST")) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *remote = json_str(jb, "remote", "");
        char dir[600], local[900];
        char err[512] = "";
        uint64_t bytes = 0;
        int rc;
        snprintf(dir, sizeof(dir), "%sdownloads", nd_config_dir());
        nd_mkdir_p(dir);
        snprintf(local, sizeof(local), "%s/smb-%llu-%s", dir,
                 (unsigned long long)s->id, path_base(remote));
        rc = nd_smb2_download(s->client, remote, local, &bytes, err, sizeof(err));
        cJSON_Delete(jb);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", rc == ND_OK);
            if (rc == ND_OK) {
                cJSON_AddStringToObject(o, "local_path", local);
                cJSON_AddStringToObject(o, "name", path_base(remote));
                cJSON_AddNumberToObject(o, "size", (double)bytes);
            } else {
                cJSON_AddStringToObject(o, "detail", err);
            }
            nd_http_json(req->fd, rc == ND_OK ? 200 : 502, o);
            cJSON_Delete(o);
        }
        nd_smb_sess_put(s);
        return 1;
    }

    if (!strcmp(rest, "/upload") && !strcmp(req->method, "POST")) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *remote = json_str(jb, "remote", "");
        const char *b64 = json_str(jb, "data_base64", "");
        size_t cap = (strlen(b64) * 3) / 4 + 4;
        unsigned char *data = (unsigned char *)malloc(cap);
        int n = data ? nd_base64_decode(b64, data, cap) : -1;
        char dir[600], local[900];
        char err[512] = "";
        uint64_t bytes = 0;
        int rc = ND_E_PARAM;
        if (n < 0) {
            free(data);
            cJSON_Delete(jb);
            nd_http_error_json(req->fd, 400, ND_E_PARAM, "data_base64 非法");
            nd_smb_sess_put(s);
            return 1;
        }
        snprintf(dir, sizeof(dir), "%suploads", nd_config_dir());
        nd_mkdir_p(dir);
        snprintf(local, sizeof(local), "%s/smb-%llu-%s", dir,
                 (unsigned long long)s->id, path_base(remote));
        if (nd_write_file(local, data, (size_t)n) == 0)
            rc = nd_smb2_upload(s->client, local, remote, &bytes, err, sizeof(err));
        free(data);
        cJSON_Delete(jb);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", rc == ND_OK);
            if (rc == ND_OK)
                cJSON_AddNumberToObject(o, "size", (double)bytes);
            else
                cJSON_AddStringToObject(o, "detail", err);
            nd_http_json(req->fd, rc == ND_OK ? 200 : 502, o);
            cJSON_Delete(o);
        }
        nd_smb_sess_put(s);
        return 1;
    }

    if (!strcmp(rest, "") && !strcmp(req->method, "DELETE")) {
        nd_smb_sess_free(s);
        nd_smb_sess_put(s);
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
            nd_http_json(req->fd, 200, o);
            cJSON_Delete(o);
        }
        return 1;
    }

    nd_smb_sess_put(s);
    return 0;
}

static void interfaces_json(int fd){
    nd_iface ifs[ND_MAX_IFACE];
    size_t n = 0;
    cJSON *arr;

    if (nd_iface_list(ifs, ND_MAX_IFACE, &n) != 0) {
        nd_http_error_json(fd, 500, ND_E_INTERNAL, "enum interfaces failed");
        return;
    }
    arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", ifs[i].id);
        cJSON_AddStringToObject(o, "name", ifs[i].name);
        cJSON_AddStringToObject(o, "mac", ifs[i].mac);
        cJSON_AddNumberToObject(o, "index", (double)ifs[i].index);
        cJSON_AddBoolToObject(o, "up", ifs[i].up);
        cJSON_AddNumberToObject(o, "mtu", (double)ifs[i].mtu);
        {
            cJSON *aa = cJSON_AddArrayToObject(o, "addrs");
            for (size_t j = 0; j < ifs[i].n_addr; j++) {
                char a[80];
                cJSON *x = cJSON_CreateObject();
                nd_addr_format(&ifs[i].addrs[j], 0, a, sizeof(a));
                cJSON_AddStringToObject(x, "addr", a);
                cJSON_AddStringToObject(x, "family",
                                        ifs[i].addrs[j].family == AF_INET ? "v4" : "v6");
                cJSON_AddNumberToObject(x, "prefix", ifs[i].addrs[j].prefix);
                cJSON_AddNumberToObject(x, "scope", (double)ifs[i].addrs[j].scope_id);
                cJSON_AddItemToArray(aa, x);
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    nd_http_json(fd, 200, arr);
    cJSON_Delete(arr);
}

static void test_bind(int fd, nd_http_req *req)
{
    cJSON *body = cJSON_Parse(req->body ? req->body : "");
    nd_bind_spec b;
    nd_iface ifs[ND_MAX_IFACE];
    size_t n = 0;
    nd_plan plan;
    char err[256] = "";
    int rc;
    cJSON *o;

    if (!body) {
        nd_http_error_json(fd, 400, ND_E_PARAM, "body 不是合法 JSON");
        return;
    }
    rc = nd_bind_from_json(body, &b, err, sizeof(err));
    if (rc != ND_OK) {
        cJSON_Delete(body);
        nd_http_error_json(fd, 400, rc, err);
        return;
    }
    nd_iface_list(ifs, ND_MAX_IFACE, &n);
    rc = nd_plan_build(&b, ifs, n, 1, &plan);
    o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", rc == ND_OK);
    cJSON_AddNumberToObject(o, "code", rc);
    if (rc != ND_OK)
        cJSON_AddStringToObject(o, "message", nd_error_str(rc));
    add_slots(o, &plan);
    nd_plan_close(&plan);
    nd_http_json(fd, 200, o); /* 测试成功执行；能否绑定看 ok 字段 */
    cJSON_Delete(o);
    cJSON_Delete(body);
}

static int path_id(const char *path, const char *prefix, uint64_t *id, const char **rest)
{
    const char *p = path + strlen(prefix);
    char *end = NULL;
    unsigned long long v;
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0)
        return -1;
    if (*p < '0' || *p > '9')
        return -1;
    v = strtoull(p, &end, 10);
    if (!end)
        return -1;
    *id = (uint64_t)v;
    *rest = end;
    return 0;
}

int nd_api_handle(void *vreq)
{
    nd_http_req *req = (nd_http_req *)vreq;
    int is_get = !strcmp(req->method, "GET");
    int is_post = !strcmp(req->method, "POST");
    int is_put = !strcmp(req->method, "PUT");
    int is_delete = !strcmp(req->method, "DELETE");

    if (!strcmp(req->path, "/api/meta") && is_get) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "netdebug");
        cJSON_AddStringToObject(o, "version", "0.0.1");
#ifdef _WIN32
        cJSON_AddStringToObject(o, "os", "windows");
#else
        cJSON_AddStringToObject(o, "os", "linux");
#endif
        cJSON_AddNumberToObject(o, "port", nd_http_port());
        cJSON_AddNumberToObject(o, "ws_clients", nd_ws_client_count());
        nd_http_json(req->fd, 200, o);
        cJSON_Delete(o);
        return 1;
    }

    if (!strcmp(req->path, "/api/net/interfaces") && is_get) {
        interfaces_json(req->fd);
        return 1;
    }

    if (!strcmp(req->path, "/api/net/test-bind") && is_post) {
        test_bind(req->fd, req);
        return 1;
    }

    if (!strcmp(req->path, "/api/serial/ports") && is_get) {
        nd_serial_port ports[64];
        size_t n = 0;
        cJSON *arr = cJSON_CreateArray();
        nd_serial_list(ports, 64, &n);
        for (size_t i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "port", ports[i].port);
            cJSON_AddStringToObject(o, "desc", ports[i].desc);
            cJSON_AddItemToArray(arr, o);
        }
        nd_http_json(req->fd, 200, arr);
        cJSON_Delete(arr);
        return 1;
    }

    if (!strncmp(req->path, "/api/ftp", 8) &&
        (req->path[8] == '\0' || req->path[8] == '/')) {
        if (ftp_handle(req))
            return 1;
    }

    if (!strcmp(req->path, "/api/fs/download") && is_get) {
        fs_download(req);
        return 1;
    }

    if ((!strcmp(req->path, "/api/tftp/download") || !strcmp(req->path, "/api/tftp/upload")) &&
        is_post) {
        tftp_api(req);
        return 1;
    }

    if (!strncmp(req->path, "/api/smb", 8) &&
        (req->path[8] == '\0' || req->path[8] == '/')) {
        if (smb_api(req))
            return 1;
    }

    if (!strcmp(req->path, "/api/http/send") && is_post) {
        cJSON *jb = cJSON_Parse(req->body ? req->body : "");
        const char *method;
        const char *url;
        const char *body_enc;
        const char *body_text;
        int timeout_ms;
        int follow;
        nd_http_req_hdr hdrs[32];
        int n_hdrs = 0;
        unsigned char *payload = NULL;
        size_t payload_len = 0;
        nd_http_result res;
        char err[512] = "";
        int rc;
        cJSON *arr;
        cJSON *item;

        if (!jb) {
            nd_http_error_json(req->fd, 400, ND_E_PARAM, "body 不是合法 JSON");
            return 1;
        }
        method = json_str(jb, "method", "GET");
        url = json_str(jb, "url", "");
        timeout_ms = json_int(jb, "timeout_ms", 15000);
        {
            cJSON *fj = cJSON_GetObjectItemCaseSensitive(jb, "follow_redirects");
            follow = fj ? cJSON_IsTrue(fj) : 1;
        }
        if (timeout_ms < 1000) timeout_ms = 1000;
        if (timeout_ms > 120000) timeout_ms = 120000;
        if (!url[0]) {
            cJSON_Delete(jb);
            nd_http_error_json(req->fd, 400, ND_E_PARAM, "url 不能为空");
            return 1;
        }

        arr = cJSON_GetObjectItemCaseSensitive(jb, "headers");
        cJSON_ArrayForEach(item, arr) {
            cJSON *hn = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *hv = cJSON_GetObjectItemCaseSensitive(item, "value");
            if (n_hdrs >= 32)
                break;
            if (!hn || !cJSON_IsString(hn))
                continue;
            nd_strlcpy(hdrs[n_hdrs].name, hn->valuestring, sizeof(hdrs[n_hdrs].name));
            nd_strlcpy(hdrs[n_hdrs].value,
                       (hv && cJSON_IsString(hv)) ? hv->valuestring : "",
                       sizeof(hdrs[n_hdrs].value));
            n_hdrs++;
        }

        body_text = json_str(jb, "body", "");
        body_enc = json_str(jb, "body_encoding", "utf8");
        if (!strcmp(body_enc, "hex")) {
            size_t cap = strlen(body_text) / 2 + 1;
            payload = (unsigned char *)malloc(cap);
            if (payload) {
                int n = nd_hex_decode(body_text, payload, cap);
                if (n < 0) {
                    free(payload);
                    payload = NULL;
                } else {
                    payload_len = (size_t)n;
                }
            }
        } else if (!strcmp(body_enc, "base64")) {
            size_t cap = (strlen(body_text) * 3) / 4 + 4;
            payload = (unsigned char *)malloc(cap);
            if (payload) {
                int n = nd_base64_decode(body_text, payload, cap);
                if (n < 0) {
                    free(payload);
                    payload = NULL;
                } else {
                    payload_len = (size_t)n;
                }
            }
        } else {
            payload_len = strlen(body_text);
            if (payload_len) {
                payload = (unsigned char *)malloc(payload_len);
                if (payload)
                    memcpy(payload, body_text, payload_len);
            }
        }

        rc = nd_http_do(method, url, hdrs, n_hdrs, payload, payload_len,
                        timeout_ms, follow, &res, err, sizeof(err));
        free(payload);
        cJSON_Delete(jb);

        if (rc != ND_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 0);
            cJSON_AddNumberToObject(o, "code", rc);
            cJSON_AddStringToObject(o, "message", nd_error_str(rc));
            cJSON_AddStringToObject(o, "detail", err);
            nd_http_json(req->fd, rc == ND_E_PARAM ? 400 : 502, o);
            cJSON_Delete(o);
            nd_http_result_free(&res);
            return 1;
        }

        {
            cJSON *o = http_result_json(&res);
            nd_http_json(req->fd, 200, o);
            cJSON_Delete(o);
        }
        nd_http_result_free(&res);
        return 1;
    }

    if (!strcmp(req->path, "/api/system/pick-folder") && is_post) {
        char dir[600];
        int rc = nd_pick_folder(dir, sizeof(dir));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", rc == 1);
        if (rc == 1)
            cJSON_AddStringToObject(o, "path", dir);
        else
            cJSON_AddStringToObject(o, "message", rc == 0 ? "已取消" : "当前系统不支持目录选择");
        nd_http_json(req->fd, 200, o);
        cJSON_Delete(o);
        return 1;
    }

    if (!strcmp(req->path, "/api/services")) {
        if (is_get) {
            uint64_t ids[256];
            size_t n = nd_service_list(ids, 256);
            cJSON *arr = cJSON_CreateArray();
            for (size_t i = 0; i < n; i++) {
                nd_service *s = nd_service_get(ids[i]);
                if (s) {
                    cJSON_AddItemToArray(arr, service_json(s));
                    nd_service_put(s);
                }
            }
            nd_http_json(req->fd, 200, arr);
            cJSON_Delete(arr);
            return 1;
        }
        if (is_post) {
            cJSON *body = cJSON_Parse(req->body ? req->body : "");
            nd_bind_spec b;
            char err[256] = "";
            const char *name;
            const char *proto;
            int echo;
            nd_service *s;
            cJSON *o;

            if (!body) {
                nd_http_error_json(req->fd, 400, ND_E_PARAM, "body 不是合法 JSON");
                return 1;
            }
            if (nd_bind_from_json(body, &b, err, sizeof(err)) != ND_OK) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM, err);
                return 1;
            }
            name = json_str(body, "name", "raw service");
            proto = json_str(body, "proto", "raw-tcp");
            {
                cJSON *e = cJSON_GetObjectItemCaseSensitive(body, "echo");
                echo = (e && cJSON_IsBool(e)) ? (cJSON_IsTrue(e) ? 1 : 0)
                                              : json_int(body, "echo", 0);
            }
            if (strcmp(proto, "raw-tcp") != 0 && strcmp(proto, "raw-udp") != 0 &&
                strcmp(proto, "telnet") != 0 && strcmp(proto, "ftp") != 0 &&
                strcmp(proto, "tftp") != 0 && strcmp(proto, "http") != 0 &&
                strcmp(proto, "speedtest") != 0) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM,
                                   "当前版本支持 raw-tcp / raw-udp / telnet / ftp / tftp / http / speedtest 服务");
                return 1;
            }
            if (!strcmp(proto, "raw-udp") || !strcmp(proto, "tftp"))
                b.type = SOCK_DGRAM;
            else
                b.type = SOCK_STREAM;
            s = nd_service_new(name, proto, &b, echo);
            if (s && !strcmp(proto, "tftp")) {
                cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "options");
                cJSON *v;
                v = cJSON_GetObjectItemCaseSensitive(op, "root");
                if (v && cJSON_IsString(v) && v->valuestring[0])
                    nd_strlcpy(s->tftp_root, v->valuestring, sizeof(s->tftp_root));
                v = cJSON_GetObjectItemCaseSensitive(op, "readonly");
                if (v && cJSON_IsBool(v))
                    s->tftp_readonly = cJSON_IsTrue(v);
                v = cJSON_GetObjectItemCaseSensitive(op, "timeout");
                if (v && cJSON_IsNumber(v) && v->valueint >= 1 && v->valueint <= 60)
                    s->tftp_timeout = v->valueint;
            }
            if (s && !strcmp(proto, "http")) {
                cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "options");
                cJSON *v = cJSON_GetObjectItemCaseSensitive(op, "root");
                if (v && cJSON_IsString(v) && v->valuestring[0])
                    nd_strlcpy(s->http_root, v->valuestring, sizeof(s->http_root));
            }
            if (s && !strcmp(proto, "ftp")) {                cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "options");
                cJSON *v;
                v = cJSON_GetObjectItemCaseSensitive(op, "root");
                if (v && cJSON_IsString(v) && v->valuestring[0])
                    nd_strlcpy(s->ftp_root, v->valuestring, sizeof(s->ftp_root));
                v = cJSON_GetObjectItemCaseSensitive(op, "readonly");
                if (v && cJSON_IsBool(v))
                    s->ftp_readonly = cJSON_IsTrue(v);
                v = cJSON_GetObjectItemCaseSensitive(op, "anonymous");
                if (v && cJSON_IsBool(v))
                    s->ftp_anonymous = cJSON_IsTrue(v);
                v = cJSON_GetObjectItemCaseSensitive(op, "user");
                if (v && cJSON_IsString(v))
                    nd_strlcpy(s->ftp_user, v->valuestring, sizeof(s->ftp_user));
                v = cJSON_GetObjectItemCaseSensitive(op, "password");
                if (v && cJSON_IsString(v))
                    nd_strlcpy(s->ftp_pass, v->valuestring, sizeof(s->ftp_pass));
                v = cJSON_GetObjectItemCaseSensitive(op, "advertise");
                if (v && cJSON_IsString(v))
                    nd_strlcpy(s->ftp_advertise, v->valuestring, sizeof(s->ftp_advertise));
                v = cJSON_GetObjectItemCaseSensitive(op, "pasv_ports");
                if (v && cJSON_IsArray(v) && cJSON_GetArraySize(v) >= 2) {
                    int lo = cJSON_GetArrayItem(v, 0)->valueint;
                    int hi = cJSON_GetArrayItem(v, 1)->valueint;
                    if (lo > 0 && hi >= lo && hi < 65536) {
                        s->pasv_min = (uint16_t)lo;
                        s->pasv_max = (uint16_t)hi;
                    }
                }
            }
            cJSON_Delete(body);
            if (!s) {
                nd_http_error_json(req->fd, 500, ND_E_INTERNAL, "create failed");
                return 1;
            }
            nd_store_save_services();
            o = service_json(s);
            nd_http_json(req->fd, 201, o);
            cJSON_Delete(o);
            return 1;
        }
    }

    {
        uint64_t id = 0;
        const char *rest = NULL;
        if (path_id(req->path, "/api/services/", &id, &rest) == 0) {
            nd_service *s = nd_service_get(id);
            if (!s) {
                nd_http_error_json(req->fd, 404, ND_E_PARAM, "service not found");
                return 1;
            }
            if (!strcmp(rest, "") && is_put) {
                cJSON *jb = cJSON_Parse(req->body ? req->body : "");
                nd_bind_spec nb;
                char berr[256] = "";
                const char *name;
                const char *proto;
                int echo;
                int was_running = nd_service_running(s);
                if (!jb) {
                    nd_http_error_json(req->fd, 400, ND_E_PARAM, "body 不是合法 JSON");
                    nd_service_put(s);
                    return 1;
                }
                if (nd_bind_from_json(jb, &nb, berr, sizeof(berr)) != ND_OK) {
                    cJSON_Delete(jb);
                    nd_http_error_json(req->fd, 400, ND_E_PARAM, berr);
                    nd_service_put(s);
                    return 1;
                }
                name = json_str(jb, "name", s->name);
                proto = json_str(jb, "proto", s->proto);
                if (strcmp(proto, "raw-tcp") != 0 && strcmp(proto, "raw-udp") != 0 &&
                    strcmp(proto, "telnet") != 0 && strcmp(proto, "ftp") != 0 &&
                    strcmp(proto, "tftp") != 0 && strcmp(proto, "http") != 0 &&
                    strcmp(proto, "speedtest") != 0) {
                    cJSON_Delete(jb);
                    nd_http_error_json(req->fd, 400, ND_E_PARAM,
                                       "当前版本支持 raw-tcp / raw-udp / telnet / ftp / tftp / http / speedtest 服务");
                    nd_service_put(s);
                    return 1;
                }
                nb.type = (!strcmp(proto, "raw-udp") || !strcmp(proto, "tftp"))
                              ? SOCK_DGRAM : SOCK_STREAM;
                echo = json_int(jb, "echo", s->echo);
                {
                    cJSON *e = cJSON_GetObjectItemCaseSensitive(jb, "echo");
                    if (e && cJSON_IsBool(e))
                        echo = cJSON_IsTrue(e) ? 1 : 0;
                }

                if (was_running)
                    nd_service_stop(s);
                nd_strlcpy(s->name, name, sizeof(s->name));
                nd_strlcpy(s->proto, proto, sizeof(s->proto));
                s->bind = nb;
                s->echo = echo;
                {
                    cJSON *op = cJSON_GetObjectItemCaseSensitive(jb, "options");
                    if (op && cJSON_IsObject(op)) {
                        cJSON *v = cJSON_GetObjectItemCaseSensitive(op, "root");
                        if (v && cJSON_IsString(v) && v->valuestring[0]) {
                            if (!strcmp(proto, "http"))
                                nd_strlcpy(s->http_root, v->valuestring, sizeof(s->http_root));
                            else if (!strcmp(proto, "ftp"))
                                nd_strlcpy(s->ftp_root, v->valuestring, sizeof(s->ftp_root));
                            else if (!strcmp(proto, "tftp"))
                                nd_strlcpy(s->tftp_root, v->valuestring, sizeof(s->tftp_root));
                        }
                        v = cJSON_GetObjectItemCaseSensitive(op, "readonly");
                        if (v && cJSON_IsBool(v)) {
                            s->ftp_readonly = cJSON_IsTrue(v);
                            s->tftp_readonly = cJSON_IsTrue(v);
                        }
                        v = cJSON_GetObjectItemCaseSensitive(op, "anonymous");
                        if (v && cJSON_IsBool(v))
                            s->ftp_anonymous = cJSON_IsTrue(v);
                    }
                }
                cJSON_Delete(jb);
                nd_store_save_services();

                if (was_running) {
                    char serr[256] = "";
                    int rc = nd_service_start(s, serr, sizeof(serr));
                    if (rc != ND_OK) {
                        cJSON *o = cJSON_CreateObject();
                        cJSON_AddNumberToObject(o, "code", rc);
                        cJSON_AddStringToObject(o, "message", nd_error_str(rc));
                        cJSON_AddStringToObject(o, "detail", serr);
                        nd_http_json(req->fd, 409, o);
                        cJSON_Delete(o);
                        nd_service_put(s);
                        return 1;
                    }
                }
                {
                    cJSON *o = service_json(s);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                }
                nd_service_put(s);
                return 1;
            }
            if (!strcmp(rest, "/start") && is_post) {
                char err[256] = "";
                int rc = nd_service_start(s, err, sizeof(err));
                if (rc != ND_OK) {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddNumberToObject(o, "code", rc);
                    cJSON_AddStringToObject(o, "message", nd_error_str(rc));
                    cJSON_AddStringToObject(o, "detail", err);
                    if (s->plan.n)
                        add_slots(o, &s->plan);
                    nd_http_json(req->fd, 409, o);
                    cJSON_Delete(o);
                } else {
                    cJSON *o = service_json(s);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                    /* 记住“正在运行”，下次启动时自动恢复监听 */
                    nd_store_save_services();
                }
                nd_service_put(s);
                return 1;
            }
            if (!strcmp(rest, "/stop") && is_post) {
                nd_service_stop(s);
                nd_store_save_services();
                {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", 1);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                }
                nd_service_put(s);
                return 1;
            }
            if (!strcmp(rest, "/connections") && is_get) {
                uint64_t cids[512];
                size_t n = nd_conn_list(id, cids, 512);
                cJSON *arr = cJSON_CreateArray();
                for (size_t i = 0; i < n; i++) {
                    nd_conn *c = nd_conn_get(cids[i]);
                    if (c) {
                        cJSON_AddItemToArray(arr, conn_json(c));
                        nd_conn_put(c);
                    }
                }
                nd_http_json(req->fd, 200, arr);
                cJSON_Delete(arr);
                nd_service_put(s);
                return 1;
            }
            if (!strcmp(rest, "") && is_delete) {
                nd_service_free(s); /* 内部释放注册表引用 */
                nd_service_put(s);  /* 释放本次 API 查询引用 */
                nd_store_save_services();
                {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", 1);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                }
                return 1;
            }
            if (!strcmp(rest, "") && is_get) {
                cJSON *o = service_json(s);
                nd_http_json(req->fd, 200, o);
                cJSON_Delete(o);
                nd_service_put(s);
                return 1;
            }
            nd_service_put(s);
        }
    }

    {
        uint64_t id = 0;
        const char *rest = NULL;
        if (path_id(req->path, "/api/conn/", &id, &rest) == 0) {
            nd_conn *c = nd_conn_get(id);
            if (!c) {
                nd_http_error_json(req->fd, 404, ND_E_PARAM, "conn not found");
                return 1;
            }
            if (!strcmp(rest, "/send") && is_post) {
                cJSON *body = cJSON_Parse(req->body ? req->body : "");
                const char *data = json_str(body, "data", "");
                const char *enc = json_str(body, "enc", "utf8");
                int rc = -1;
                if (!strcmp(enc, "hex")) {
                    size_t cap = strlen(data) / 2 + 1;
                    unsigned char *buf = (unsigned char *)malloc(cap);
                    int n = buf ? nd_hex_decode(data, buf, cap) : -1;
                    if (n >= 0)
                        rc = nd_conn_send(c, buf, (size_t)n);
                    free(buf);
                } else {
                    rc = nd_conn_send(c, data, strlen(data));
                }
                cJSON_Delete(body);
                if (rc == 0) {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", 1);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                } else {
                    nd_http_error_json(req->fd, 409, ND_E_IO, "send failed");
                }
                nd_conn_put(c);
                return 1;
            }
            if (!strcmp(rest, "/close") && is_post) {
                nd_conn_close(c);
                {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", 1);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                }
                nd_conn_put(c);
                return 1;
            }
            if (!strcmp(rest, "") && is_get) {
                cJSON *o = conn_json(c);
                nd_http_json(req->fd, 200, o);
                cJSON_Delete(o);
                nd_conn_put(c);
                return 1;
            }
            nd_conn_put(c);
        }
    }

    if (!strcmp(req->path, "/api/sessions")) {
        if (is_get) {
            uint64_t ids[128];
            size_t n = nd_session_list(ids, 128);
            cJSON *arr = cJSON_CreateArray();
            for (size_t i = 0; i < n; i++) {
                nd_session *ss = nd_session_get(ids[i]);
                if (ss) {
                    nd_session_sync(ss);
                    cJSON_AddItemToArray(arr, session_json(ss));
                    nd_session_put(ss);
                }
            }
            nd_http_json(req->fd, 200, arr);
            cJSON_Delete(arr);
            return 1;
        }
        if (is_post) {
            cJSON *body = cJSON_Parse(req->body ? req->body : "");
            const char *host;
            const char *fams;
            const char *name;
            const char *iface;
            const char *src;
            const char *trans;
            const char *protocol;
            int udp = 0;
            int serial = 0;
            int sshflag = 0;
            int telnet = 0;
            int port;
            nd_family fam = ND_FAM_DUAL;
            nd_session *ss;

            if (!body) {
                nd_http_error_json(req->fd, 400, ND_E_PARAM, "body 不是合法 JSON");
                return 1;
            }
            host = json_str(body, "host", "");
            port = json_int(body, "port", 0);
            fams = json_str(body, "family", "dual");
            name = json_str(body, "name", host);
            iface = json_str(body, "iface_id", "");
            src = json_str(body, "src_addr", "");
            trans = json_str(body, "transport", "tcp");
            if (!strcmp(trans, "udp"))
                udp = 1;
            else if (!strcmp(trans, "serial"))
                serial = 1;
            else if (!strcmp(trans, "ssh"))
                sshflag = 1;
            else if (strcmp(trans, "tcp") != 0) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM,
                                   "transport 必须是 tcp / udp / serial / ssh");
                return 1;
            }
            protocol = json_str(body, "protocol", "raw");
            if (!strcmp(protocol, "telnet"))
                telnet = 1;
            else if (strcmp(protocol, "raw") != 0 && strcmp(protocol, "tcp") != 0 &&
                     strcmp(protocol, "serial") != 0 &&
                     !(sshflag && strcmp(protocol, "ssh") == 0)) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM,
                                   "protocol 必须是 raw / telnet / serial / ssh");
                return 1;
            }
            if (telnet && (udp || serial)) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM, "Telnet 只能基于 TCP");
                return 1;
            }

            if (serial) {
                const char *sport = json_str(body, "serial_port", "");
                if (!sport[0]) {
                    cJSON_Delete(body);
                    nd_http_error_json(req->fd, 400, ND_E_PARAM, "serial_port 不能为空");
                    return 1;
                }
                if (!name || !name[0])
                    name = sport;
                ss = nd_session_new(name, sport, 0, ND_FAM_DUAL, "", "", 0, 0);
                if (!ss) {
                    cJSON_Delete(body);
                    nd_http_error_json(req->fd, 500, ND_E_INTERNAL, "create session failed");
                    return 1;
                }
                ss->serial = 1;
                nd_strlcpy(ss->serial_port, sport, sizeof(ss->serial_port));
                ss->baud = json_int(body, "baud", 115200);
                ss->data_bits = json_int(body, "data_bits", 8);
                ss->parity = json_int(body, "parity", 0);
                ss->stop_bits = json_int(body, "stop_bits", 1);
                ss->flow = json_int(body, "flow", 0);
                ss->dtr = json_int(body, "dtr", 1);
                ss->rts = json_int(body, "rts", 1);
                cJSON_Delete(body);
                {
                    cJSON *o = session_json(ss);
                    nd_http_json(req->fd, 201, o);
                    cJSON_Delete(o);
                }
                return 1;
            }

            if (!host[0]) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM, "host 不能为空");
                return 1;
            }
            if (port <= 0 || port > 65535) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM_PORT, "端口必须在 1-65535");
                return 1;
            }
            if (!strcmp(fams, "v4") || !strcmp(fams, "ipv4"))
                fam = ND_FAM_V4;
            else if (!strcmp(fams, "v6") || !strcmp(fams, "ipv6"))
                fam = ND_FAM_V6;
            else if (strcmp(fams, "dual") != 0) {
                cJSON_Delete(body);
                nd_http_error_json(req->fd, 400, ND_E_PARAM_FAMILY,
                                   "family 必须是 v4 / v6 / dual");
                return 1;
            }

            ss = nd_session_new(name, host, (uint16_t)port, fam, iface, src, udp, telnet);
            if (ss && sshflag) {
                ss->ssh = 1;
                nd_strlcpy(ss->ssh_user, json_str(body, "user", "root"), sizeof(ss->ssh_user));
                nd_strlcpy(ss->ssh_pass, json_str(body, "password", ""), sizeof(ss->ssh_pass));
                nd_strlcpy(ss->ssh_key, json_str(body, "key_path", ""), sizeof(ss->ssh_key));
                ss->ssh_compat = json_int(body, "compat", 0);
                ss->ssh_cols = json_int(body, "cols", 120);
                ss->ssh_rows = json_int(body, "rows", 32);
            }
            cJSON_Delete(body);
            if (!ss) {
                nd_http_error_json(req->fd, 500, ND_E_INTERNAL, "create session failed");
                return 1;
            }
            {
                cJSON *o = session_json(ss);
                nd_http_json(req->fd, 201, o);
                cJSON_Delete(o);
            }
            return 1;
        }
    }

    {
        uint64_t sid = 0;
        const char *rest = NULL;
        if (path_id(req->path, "/api/sessions/", &sid, &rest) == 0) {
            nd_session *ss = nd_session_get(sid);
            if (!ss) {
                nd_http_error_json(req->fd, 404, ND_E_PARAM, "session not found");
                return 1;
            }
            if (!strcmp(rest, "/connect") && is_post) {
                uint64_t cid = 0;
                char serr[256] = "";
                int rc = nd_session_connect(ss, &cid, serr, sizeof(serr));
                if (rc != ND_OK) {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddNumberToObject(o, "code", rc);
                    cJSON_AddStringToObject(o, "message", nd_error_str(rc));
                    cJSON_AddStringToObject(o, "detail", serr);
                    nd_http_json(req->fd, 409, o);
                    cJSON_Delete(o);
                } else {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", 1);
                    cJSON_AddNumberToObject(o, "conn_id", (double)cid);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                }
                nd_session_put(ss);
                return 1;
            }
            if (!strcmp(rest, "/disconnect") && is_post) {
                nd_session_disconnect(ss);
                {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", 1);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                }
                nd_session_put(ss);
                return 1;
            }
            if (!strcmp(rest, "") && is_get) {
                cJSON *o;
                nd_session_sync(ss);
                o = session_json(ss);
                nd_http_json(req->fd, 200, o);
                cJSON_Delete(o);
                nd_session_put(ss);
                return 1;
            }
            if (!strcmp(rest, "") && is_delete) {
                nd_session_free(ss);
                nd_session_put(ss);
                {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddBoolToObject(o, "ok", 1);
                    nd_http_json(req->fd, 200, o);
                    cJSON_Delete(o);
                }
                return 1;
            }
            nd_session_put(ss);
        }
    }

    return 0;
}
