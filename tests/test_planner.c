#include "test_framework.h"

#include "core/error.h"
#include "cJSON.h"
#include "net/bind_json.h"
#include "net/planner.h"

#include <stdio.h>
#include <string.h>

static void set_v4(nd_addr *a, const char *text)
{
    memset(a, 0, sizeof(*a));
    nd_addr_parse(text, 0, a);
    a->port = 0;
}

static void mk_ifaces(nd_iface *ifs, size_t *n)
{
    memset(ifs, 0, sizeof(nd_iface) * 2);

    strcpy(ifs[0].id, "test:eth0");
    strcpy(ifs[0].name, "eth0");
    ifs[0].up = 1;
    ifs[0].n_addr = 2;
    set_v4(&ifs[0].addrs[0], "192.168.1.5");
    ifs[0].addrs[0].prefix = 24;
    memset(&ifs[0].addrs[1], 0, sizeof(nd_addr));
    ifs[0].addrs[1].family = AF_INET6;
    ifs[0].addrs[1].scope_id = 2;
    ifs[0].addrs[1].raw[0] = 0xfe;
    ifs[0].addrs[1].raw[1] = 0x80;
    ifs[0].addrs[1].raw[15] = 0x01;

    /* 只有 IPv4 的网卡，用来验证地址族缺失的处理 */
    strcpy(ifs[1].id, "test:lo");
    strcpy(ifs[1].name, "lo");
    ifs[1].up = 1;
    ifs[1].n_addr = 1;
    set_v4(&ifs[1].addrs[0], "127.0.0.1");

    *n = 2;
}

static void case_family_name(void)
{
    BEGIN_CASE("地址族名称");

    CHECK_STR(nd_family_name(ND_FAM_V4), "v4");
    CHECK_STR(nd_family_name(ND_FAM_V6), "v6");
    CHECK_STR(nd_family_name(ND_FAM_DUAL), "dual");
    CHECK_STR(nd_family_name((nd_family)99), "?");

    END_CASE();
}

static void case_any(void)
{
    nd_iface ifs[2];
    size_t n_ifs = 0;
    nd_bind_spec spec;
    nd_plan plan;
    char text[256];

    BEGIN_CASE("绑定全部地址");

    mk_ifaces(ifs, &n_ifs);

    memset(&spec, 0, sizeof(spec));
    spec.kind = ND_BIND_ANY;
    spec.family = ND_FAM_V4;
    spec.port = 9000;
    memset(&plan, 0, sizeof(plan));
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK_INT(plan.slot[0].local.family, AF_INET);
    CHECK_INT(plan.slot[0].local.port, 9000);
    CHECK_INT(plan.slot[0].fd, -1);
    CHECK_INT(plan.degraded, 0);

    /* 未真正绑定，格式化时用 ! 标记 */
    nd_plan_format(&plan, text, sizeof(text));
    CHECK_STR(text, "!0.0.0.0:9000");

    spec.family = ND_FAM_V6;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK_INT(plan.slot[0].local.family, AF_INET6);

    /* 双栈走单个 IPv6 socket（关闭 V6ONLY） */
    spec.family = ND_FAM_DUAL;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK_INT(plan.slot[0].local.family, AF_INET6);

    END_CASE();
}

static void case_iface(void)
{
    nd_iface ifs[2];
    size_t n_ifs = 0;
    nd_bind_spec spec;
    nd_plan plan;

    BEGIN_CASE("绑定指定网卡");

    mk_ifaces(ifs, &n_ifs);

    memset(&spec, 0, sizeof(spec));
    spec.kind = ND_BIND_IFACE;
    spec.family = ND_FAM_DUAL;
    spec.port = 9000;
    strcpy(spec.iface_id, "test:eth0");

    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 2);
    CHECK_INT(plan.slot[0].local.family, AF_INET);
    CHECK_INT(plan.slot[1].local.family, AF_INET6);
    CHECK_INT(plan.degraded, 0);

    spec.family = ND_FAM_V4;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK_INT(plan.slot[0].local.family, AF_INET);

    spec.family = ND_FAM_V6;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK_INT(plan.slot[0].local.family, AF_INET6);

    /* 网卡不存在 */
    strcpy(spec.iface_id, "test:nope");
    spec.family = ND_FAM_V4;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_E_PARAM_IFACE);
    CHECK_INT(plan.fatal, 1);

    END_CASE();
}

static void case_iface_partial(void)
{
    nd_iface ifs[2];
    size_t n_ifs = 0;
    nd_bind_spec spec;
    nd_plan plan;

    BEGIN_CASE("网卡缺少地址族时的降级/失败");

    mk_ifaces(ifs, &n_ifs);

    memset(&spec, 0, sizeof(spec));
    spec.kind = ND_BIND_IFACE;
    spec.port = 9000;
    strcpy(spec.iface_id, "test:lo");   /* 只有 IPv4 */

    /* 要求双栈：降级但仍可用 */
    spec.family = ND_FAM_DUAL;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK_INT(plan.partial_family, 1);
    CHECK_INT(plan.degraded, 1);

    /* 要求 IPv6：没有地址，直接失败 */
    spec.family = ND_FAM_V6;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_E_BIND_NOADDR);
    CHECK_INT(plan.fatal, 1);

    END_CASE();
}

static void case_addr_spec(void)
{
    nd_iface ifs[2];
    size_t n_ifs = 0;
    nd_bind_spec spec;
    nd_plan plan;

    BEGIN_CASE("绑定指定 IP 的校验");

    mk_ifaces(ifs, &n_ifs);

    memset(&spec, 0, sizeof(spec));
    spec.kind = ND_BIND_ADDR;
    spec.family = ND_FAM_V4;
    spec.port = 9000;
    set_v4(&spec.addr, "192.168.1.5");
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK_INT(plan.slot[0].local.port, 9000);
    CHECK_INT(plan.slot[0].local.family, AF_INET);

    /* 具体 IP + 双栈不成立 */
    spec.family = ND_FAM_DUAL;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_E_BIND_FAMILY);
    CHECK_INT(plan.fatal, 1);

    /* 地址族与地址不匹配 */
    spec.family = ND_FAM_V6;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_E_PARAM_FAMILY);

    /* 非法地址族 */
    spec.family = (nd_family)0;
    CHECK_INT(nd_plan_build(&spec, ifs, n_ifs, 0, &plan), ND_E_PARAM_FAMILY);

    /* 空 spec */
    CHECK_INT(nd_plan_build(NULL, ifs, n_ifs, 0, &plan), ND_E_PARAM);

    END_CASE();
}

static void case_real_bind(void)
{
    nd_bind_spec spec;
    nd_plan plan;
    char text[256];

    BEGIN_CASE("真实绑定 socket");

    memset(&spec, 0, sizeof(spec));
    spec.kind = ND_BIND_ADDR;
    spec.family = ND_FAM_V4;
    spec.port = 0;                      /* 系统分配端口 */
    spec.type = SOCK_STREAM;
    set_v4(&spec.addr, "127.0.0.1");

    memset(&plan, 0, sizeof(plan));
    CHECK_INT(nd_plan_build(&spec, NULL, 0, 1, &plan), ND_OK);
    CHECK_INT(plan.n, 1);
    CHECK(plan.slot[0].fd >= 0);
    CHECK(plan.slot[0].local.port != 0);
    CHECK_INT(plan.slot[0].err, 0);

    nd_plan_format(&plan, text, sizeof(text));
    CHECK(strstr(text, "127.0.0.1:") != NULL);
    CHECK(strchr(text, '!') == NULL);   /* 已绑定，不带失败标记 */

    nd_plan_close(&plan);
    CHECK_INT(plan.slot[0].fd, -1);

    /* 绑定一个本机不存在的地址必须失败 */
    memset(&spec, 0, sizeof(spec));
    spec.kind = ND_BIND_ADDR;
    spec.family = ND_FAM_V4;
    spec.type = SOCK_STREAM;
    set_v4(&spec.addr, "240.0.0.1");
    memset(&plan, 0, sizeof(plan));
    CHECK(nd_plan_build(&spec, NULL, 0, 1, &plan) != ND_OK);
    CHECK_INT(plan.fatal, 1);
    CHECK_INT(plan.err, ND_E_BIND_NOADDR);
    CHECK_INT(plan.slot[0].fd, -1);

    END_CASE();
}

static void case_bind_json(void)
{
    cJSON *obj;
    nd_bind_spec spec;
    char err[256];
    cJSON *out;

    BEGIN_CASE("绑定 JSON 解析/序列化");

    /* any + v4 */
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "kind", "any");
    cJSON_AddStringToObject(obj, "family", "v4");
    cJSON_AddNumberToObject(obj, "port", 9000);
    memset(&spec, 0, sizeof(spec));
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_OK);
    CHECK_INT(spec.kind, ND_BIND_ANY);
    CHECK_INT(spec.family, ND_FAM_V4);
    CHECK_INT(spec.port, 9000);
    CHECK_INT(spec.type, SOCK_STREAM);
    cJSON_Delete(obj);

    /* ipv6 别名 + udp 传输 */
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "kind", "any");
    cJSON_AddStringToObject(obj, "family", "ipv6");
    cJSON_AddNumberToObject(obj, "port", 69);
    cJSON_AddStringToObject(obj, "transport", "udp");
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_OK);
    CHECK_INT(spec.family, ND_FAM_V6);
    CHECK_INT(spec.type, SOCK_DGRAM);
    cJSON_Delete(obj);

    /* addr 方式，地址带入 */
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "kind", "addr");
    cJSON_AddStringToObject(obj, "family", "v4");
    cJSON_AddNumberToObject(obj, "port", 8080);
    cJSON_AddStringToObject(obj, "addr", "127.0.0.1");
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_OK);
    CHECK_INT(spec.kind, ND_BIND_ADDR);
    CHECK_INT(spec.addr.family, AF_INET);
    CHECK_INT(spec.addr.port, 8080);
    cJSON_Delete(obj);

    /* 嵌套在 bind 字段里（创建服务时的格式） */
    obj = cJSON_CreateObject();
    {
        cJSON *b = cJSON_AddObjectToObject(obj, "bind");
        cJSON_AddStringToObject(b, "kind", "iface");
        cJSON_AddStringToObject(b, "family", "dual");
        cJSON_AddNumberToObject(b, "port", 5000);
        cJSON_AddStringToObject(b, "iface_id", "test:eth0");
    }
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_OK);
    CHECK_INT(spec.kind, ND_BIND_IFACE);
    CHECK_INT(spec.family, ND_FAM_DUAL);
    CHECK_STR(spec.iface_id, "test:eth0");
    cJSON_Delete(obj);

    /* 非法 family / kind / port / addr */
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "family", "ipx");
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_E_PARAM_FAMILY);
    cJSON_Delete(obj);

    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "kind", "bogus");
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_E_PARAM);
    cJSON_Delete(obj);

    obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "port", 70000);
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_E_PARAM_PORT);
    cJSON_Delete(obj);

    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "kind", "addr");
    cJSON_AddStringToObject(obj, "family", "v4");
    cJSON_AddStringToObject(obj, "addr", "not-an-ip");
    CHECK_INT(nd_bind_from_json(obj, &spec, err, sizeof(err)), ND_E_PARAM);
    cJSON_Delete(obj);

    /* to_json → from_json 往返 */
    memset(&spec, 0, sizeof(spec));
    spec.kind = ND_BIND_ADDR;
    spec.family = ND_FAM_V4;
    spec.port = 8443;
    set_v4(&spec.addr, "10.1.2.3");
    spec.addr.port = 8443;
    out = nd_bind_to_json(&spec);
    CHECK(out != NULL);
    if (out) {
        nd_bind_spec back;
        memset(&back, 0, sizeof(back));
        CHECK_INT(nd_bind_from_json(out, &back, err, sizeof(err)), ND_OK);
        CHECK_INT(back.kind, ND_BIND_ADDR);
        CHECK_INT(back.family, ND_FAM_V4);
        CHECK_INT(back.port, 8443);
        CHECK(nd_addr_equal(&back.addr, &spec.addr));
        cJSON_Delete(out);
    }

    END_CASE();
}

void suite_net_planner(void)
{
    SUITE("net.planner");
    case_family_name();
    case_any();
    case_iface();
    case_iface_partial();
    case_addr_spec();
    case_real_bind();
    case_bind_json();
}
