#include "session/store.h"
#include "session/service.h"
#include "core/error.h"
#include "core/log.h"
#include "core/util.h"
#include "net/bind_json.h"
#include "platform/platform.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void store_path(char *buf, size_t cap, const char *name)
{
    snprintf(buf, cap, "%s%s", nd_config_dir(), name);
}

void nd_store_save_services(void)
{
    uint64_t ids[256];
    size_t n = nd_service_list(ids, 256);
    cJSON *arr = cJSON_CreateArray();
    char path[600], tmp[620];
    char *text;

    if (!arr)
        return;
    for (size_t i = 0; i < n; i++) {
        nd_service *s = nd_service_get(ids[i]);
        if (!s)
            continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddStringToObject(o, "proto", s->proto);
        cJSON_AddStringToObject(o, "transport",
                                s->bind.type == SOCK_DGRAM ? "udp" : "tcp");
        cJSON_AddBoolToObject(o, "echo", s->echo);
        cJSON_AddBoolToObject(o, "autostart", nd_service_running(s));
        cJSON_AddItemToObject(o, "bind", nd_bind_to_json(&s->bind));
        cJSON_AddNumberToObject(o, "max_conns", (double)s->max_conns);
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
        cJSON_AddItemToArray(arr, o);
        nd_service_put(s);
    }

    text = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (!text)
        return;

    store_path(path, sizeof(path), "services.json");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (nd_write_file(tmp, text, strlen(text)) == 0) {
        remove(path);
        rename(tmp, path);
    } else {
        ND_LOGW("cannot write %s", path);
    }
    free(text);
}

void nd_store_load_services(void)
{
    char path[600];
    void *data = NULL;
    size_t len = 0;
    cJSON *arr;
    const cJSON *item;

    store_path(path, sizeof(path), "services.json");
    if (nd_read_file(path, &data, &len) != 0)
        return;

    arr = cJSON_Parse((const char *)data);
    free(data);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        ND_LOGW("services.json is not a JSON array");
        return;
    }

    cJSON_ArrayForEach(item, arr) {
        nd_bind_spec bind;
        char err[256] = "";
        const char *name = "service";
        const char *proto = "raw-tcp";
        cJSON *v;

        v = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (v && cJSON_IsString(v))
            name = v->valuestring;
        v = cJSON_GetObjectItemCaseSensitive(item, "proto");
        if (v && cJSON_IsString(v))
            proto = v->valuestring;

        if (nd_bind_from_json((cJSON *)item, &bind, err, sizeof(err)) != ND_OK) {
            ND_LOGW("skip service %s: %s", name, err);
            continue;
        }
        {
            int echo = 0;
            nd_service *s;
            v = cJSON_GetObjectItemCaseSensitive(item, "echo");
            if (v && cJSON_IsBool(v))
                echo = cJSON_IsTrue(v);
            s = nd_service_new(name, proto, &bind, echo);
            if (s && !strcmp(proto, "ftp")) {
                cJSON *op = cJSON_GetObjectItemCaseSensitive(item, "options");
                cJSON *x;
                x = cJSON_GetObjectItemCaseSensitive(op, "root");
                if (x && cJSON_IsString(x) && x->valuestring[0])
                    nd_strlcpy(s->ftp_root, x->valuestring, sizeof(s->ftp_root));
                x = cJSON_GetObjectItemCaseSensitive(op, "readonly");
                if (x && cJSON_IsBool(x))
                    s->ftp_readonly = cJSON_IsTrue(x);
                x = cJSON_GetObjectItemCaseSensitive(op, "anonymous");
                if (x && cJSON_IsBool(x))
                    s->ftp_anonymous = cJSON_IsTrue(x);
                x = cJSON_GetObjectItemCaseSensitive(op, "user");
                if (x && cJSON_IsString(x))
                    nd_strlcpy(s->ftp_user, x->valuestring, sizeof(s->ftp_user));
                x = cJSON_GetObjectItemCaseSensitive(op, "advertise");
                if (x && cJSON_IsString(x))
                    nd_strlcpy(s->ftp_advertise, x->valuestring, sizeof(s->ftp_advertise));
                x = cJSON_GetObjectItemCaseSensitive(op, "pasv_ports");
                if (x && cJSON_IsArray(x) && cJSON_GetArraySize(x) >= 2) {
                    int lo = cJSON_GetArrayItem(x, 0)->valueint;
                    int hi = cJSON_GetArrayItem(x, 1)->valueint;
                    if (lo > 0 && hi >= lo && hi < 65536) {
                        s->pasv_min = (uint16_t)lo;
                        s->pasv_max = (uint16_t)hi;
                    }
                }
            }
            if (s && !strcmp(proto, "tftp")) {
                cJSON *op = cJSON_GetObjectItemCaseSensitive(item, "options");
                cJSON *x = cJSON_GetObjectItemCaseSensitive(op, "root");
                if (x && cJSON_IsString(x) && x->valuestring[0])
                    nd_strlcpy(s->tftp_root, x->valuestring, sizeof(s->tftp_root));
                x = cJSON_GetObjectItemCaseSensitive(op, "readonly");
                if (x && cJSON_IsBool(x))
                    s->tftp_readonly = cJSON_IsTrue(x);
                x = cJSON_GetObjectItemCaseSensitive(op, "timeout");
                if (x && cJSON_IsNumber(x) && x->valueint >= 1 && x->valueint <= 60)
                    s->tftp_timeout = x->valueint;
            }
            if (s && !strcmp(proto, "http")) {
                cJSON *op = cJSON_GetObjectItemCaseSensitive(item, "options");
                cJSON *x = cJSON_GetObjectItemCaseSensitive(op, "root");
                if (x && cJSON_IsString(x) && x->valuestring[0])
                    nd_strlcpy(s->http_root, x->valuestring, sizeof(s->http_root));
            }
            /* 上次退出时在运行的服务，恢复后自动重新监听 */
            if (s) {
                cJSON *as = cJSON_GetObjectItemCaseSensitive(item, "autostart");
                if (as && cJSON_IsBool(as) && cJSON_IsTrue(as)) {
                    char serr[256] = "";
                    if (nd_service_start(s, serr, sizeof(serr)) != ND_OK)
                        ND_LOGW("autostart %s failed: %s", name, serr);
                    else
                        ND_LOGI("autostart %s", name);
                }
            }
        }
    }
    cJSON_Delete(arr);
    ND_LOGI("services restored from %s", path);
}
