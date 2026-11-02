#include "net/bind_json.h"
#include "core/error.h"
#include "core/util.h"

#include <stdio.h>
#include <string.h>

static const char *str_of(cJSON *o, const char *key, const char *def)
{
    cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : def;
}

static int int_of(cJSON *o, const char *key, int def)
{
    cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return (v && cJSON_IsNumber(v)) ? v->valueint : def;
}

static int family_from(const char *s, nd_family *out)
{
    if (!s || !strcmp(s, "dual")) {
        *out = ND_FAM_DUAL;
        return 0;
    }
    if (!strcmp(s, "v4") || !strcmp(s, "ipv4")) {
        *out = ND_FAM_V4;
        return 0;
    }
    if (!strcmp(s, "v6") || !strcmp(s, "ipv6")) {
        *out = ND_FAM_V6;
        return 0;
    }
    return -1;
}

cJSON *nd_bind_to_json(const nd_bind_spec *b)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind",
                            b->kind == ND_BIND_ANY ? "any" :
                            b->kind == ND_BIND_IFACE ? "iface" : "addr");
    cJSON_AddStringToObject(o, "family", nd_family_name(b->family));
    cJSON_AddNumberToObject(o, "port", b->port);
    cJSON_AddStringToObject(o, "iface_id", b->iface_id);
    if (b->kind == ND_BIND_ADDR) {
        char a[80];
        nd_addr_format(&b->addr, 0, a, sizeof(a));
        cJSON_AddStringToObject(o, "addr", a);
    }
    return o;
}

int nd_bind_from_json(cJSON *obj, nd_bind_spec *b, char *err, size_t errlen)
{
    cJSON *bo = cJSON_GetObjectItemCaseSensitive(obj, "bind");
    const char *kind;
    const char *fam;
    int port;

    if (!bo)
        bo = obj; /* test-bind 允许直接传 bind 对象 */
    kind = str_of(bo, "kind", "any");
    fam = str_of(bo, "family", "dual");
    port = int_of(bo, "port", 0);

    memset(b, 0, sizeof(*b));
    b->port = (uint16_t)port;
    b->type = SOCK_STREAM;

    if (family_from(fam, &b->family) != 0) {
        snprintf(err, errlen, "family 必须是 v4 / v6 / dual");
        return ND_E_PARAM_FAMILY;
    }
    if (!strcmp(kind, "any"))
        b->kind = ND_BIND_ANY;
    else if (!strcmp(kind, "iface"))
        b->kind = ND_BIND_IFACE;
    else if (!strcmp(kind, "addr"))
        b->kind = ND_BIND_ADDR;
    else {
        snprintf(err, errlen, "bind.kind 必须是 any / iface / addr");
        return ND_E_PARAM;
    }
    if (port < 0 || port > 65535) {
        snprintf(err, errlen, "端口必须在 0-65535");
        return ND_E_PARAM_PORT;
    }
    nd_strlcpy(b->iface_id, str_of(bo, "iface_id", ""), sizeof(b->iface_id));
    if (b->kind == ND_BIND_ADDR) {
        const char *ad = str_of(bo, "addr", "");
        if (nd_addr_parse(ad, port, &b->addr) != 0) {
            snprintf(err, errlen, "addr 非法: %s", ad);
            return ND_E_PARAM;
        }
        b->addr.port = (uint16_t)port;
    }
    if (!strcmp(str_of(obj, "transport", "tcp"), "udp"))
        b->type = SOCK_DGRAM;
    return ND_OK;
}
