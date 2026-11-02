#include "test_framework.h"

#include "core/error.h"
#include "net/addr.h"
#include "net/resolver.h"

#include <string.h>

static void fill_v4(nd_addr *a, const unsigned char ip[4], uint16_t port)
{
    memset(a, 0, sizeof(*a));
    a->family = AF_INET;
    a->port = port;
    memcpy(a->raw, ip, 4);
}

static void fill_v6(nd_addr *a, const unsigned char ip[16], uint16_t port)
{
    memset(a, 0, sizeof(*a));
    a->family = AF_INET6;
    a->port = port;
    memcpy(a->raw, ip, 16);
}

static void case_parse_v4(void)
{
    nd_addr a;
    char out[96];

    BEGIN_CASE("IPv4 解析/格式化");

    CHECK_INT(nd_addr_parse("127.0.0.1:8080", 0, &a), 0);
    CHECK_INT(a.family, AF_INET);
    CHECK_INT(a.port, 8080);
    CHECK_INT(nd_addr_format(&a, 1, out, sizeof(out)), 0);
    CHECK_STR(out, "127.0.0.1:8080");

    CHECK_INT(nd_addr_format(&a, 0, out, sizeof(out)), 0);
    CHECK_STR(out, "127.0.0.1");

    /* 缺省端口 */
    CHECK_INT(nd_addr_parse("10.0.0.1", 443, &a), 0);
    CHECK_INT(a.port, 443);

    /* 首尾空白容忍 */
    CHECK_INT(nd_addr_parse("  10.0.0.1:80  ", 0, &a), 0);
    CHECK_INT(a.port, 80);
    CHECK_INT(nd_addr_format(&a, 0, out, sizeof(out)), 0);
    CHECK_STR(out, "10.0.0.1");

    /* 端口 0（系统分配）合法 */
    CHECK_INT(nd_addr_parse("0.0.0.0:0", 9000, &a), 0);
    CHECK_INT(a.port, 0);
    CHECK(nd_addr_is_any(&a));

    END_CASE();
}

static void case_parse_v6(void)
{
    nd_addr a;
    char out[96];

    BEGIN_CASE("IPv6 解析/格式化");

    CHECK_INT(nd_addr_parse("[2001:db8::1]:8443", 0, &a), 0);
    CHECK_INT(a.family, AF_INET6);
    CHECK_INT(a.port, 8443);
    CHECK_INT(nd_addr_format(&a, 1, out, sizeof(out)), 0);
    CHECK_STR(out, "[2001:db8::1]:8443");

    CHECK_INT(nd_addr_format(&a, 0, out, sizeof(out)), 0);
    CHECK_STR(out, "2001:db8::1");

    /* 裸 IPv6 + 缺省端口 */
    CHECK_INT(nd_addr_parse("::1", 9000, &a), 0);
    CHECK_INT(a.family, AF_INET6);
    CHECK_INT(a.port, 9000);
    CHECK(nd_addr_is_loopback(&a));

    /* 链路本地 + scope id */
    CHECK_INT(nd_addr_parse("fe80::1%12", 0, &a), 0);
    CHECK_INT(a.scope_id, 12);
    CHECK(nd_addr_is_linklocal(&a));
    CHECK_INT(nd_addr_format(&a, 1, out, sizeof(out)), 0);
    CHECK_STR(out, "[fe80::1%12]:0");

    END_CASE();
}

static void case_parse_invalid(void)
{
    nd_addr a;

    BEGIN_CASE("非法地址拒绝");

    CHECK(nd_addr_parse("", 0, &a) != 0);
    CHECK(nd_addr_parse("999.1.1.1", 0, &a) != 0);
    CHECK(nd_addr_parse("[::1]:99999", 0, &a) != 0);
    CHECK(nd_addr_parse("[::1", 0, &a) != 0);        /* 缺 ] */
    CHECK(nd_addr_parse("1.2.3.4:abc", 0, &a) != 0);
    CHECK(nd_addr_parse("1.2.3.4:80:90", 0, &a) != 0);
    CHECK(nd_addr_parse(NULL, 0, &a) != 0);
    CHECK(nd_addr_parse("1.2.3.4", 0, NULL) != 0);

    END_CASE();
}

static void case_equal(void)
{
    nd_addr a, b;
    static const unsigned char ip1[4] = {10, 1, 2, 3};
    static const unsigned char ip2[4] = {10, 1, 2, 4};

    BEGIN_CASE("地址比较");

    fill_v4(&a, ip1, 80);
    fill_v4(&b, ip1, 80);
    CHECK(nd_addr_equal(&a, &b));

    fill_v4(&b, ip1, 81);
    CHECK(!nd_addr_equal(&a, &b));   /* 端口不同 */

    fill_v4(&b, ip2, 80);
    CHECK(!nd_addr_equal(&a, &b));   /* 地址不同 */

    fill_v6(&b, a.raw, 80);
    b.raw[15] = 1;
    CHECK(!nd_addr_equal(&a, &b));   /* 地址族不同 */

    END_CASE();
}

static void case_classify(void)
{
    nd_addr a;

    BEGIN_CASE("地址分类 any/loopback/linklocal");

    CHECK_INT(nd_addr_parse("0.0.0.0", 0, &a), 0);
    CHECK(nd_addr_is_any(&a));
    CHECK(!nd_addr_is_loopback(&a));

    CHECK_INT(nd_addr_parse("::", 0, &a), 0);
    CHECK(nd_addr_is_any(&a));

    CHECK_INT(nd_addr_parse("127.0.0.5", 0, &a), 0);
    CHECK(nd_addr_is_loopback(&a));
    CHECK_INT(nd_addr_parse("10.0.0.1", 0, &a), 0);
    CHECK(!nd_addr_is_loopback(&a));

    CHECK_INT(nd_addr_parse("::1", 0, &a), 0);
    CHECK(nd_addr_is_loopback(&a));
    CHECK_INT(nd_addr_parse("2001:db8::1", 0, &a), 0);
    CHECK(!nd_addr_is_loopback(&a));

    CHECK_INT(nd_addr_parse("169.254.10.20", 0, &a), 0);
    CHECK(nd_addr_is_linklocal(&a));
    CHECK_INT(nd_addr_parse("192.168.1.1", 0, &a), 0);
    CHECK(!nd_addr_is_linklocal(&a));

    CHECK_INT(nd_addr_parse("fe80::abcd", 0, &a), 0);
    CHECK(nd_addr_is_linklocal(&a));
    CHECK_INT(nd_addr_parse("2001:db8::1", 0, &a), 0);
    CHECK(!nd_addr_is_linklocal(&a));

    END_CASE();
}

static void case_sockaddr_roundtrip(void)
{
    nd_addr a, back;
    struct sockaddr_storage ss;
    int salen = 0;

    BEGIN_CASE("sockaddr 双向转换");

    memset(&a, 0, sizeof(a));
    CHECK_INT(nd_addr_parse("192.168.1.9:1234", 0, &a), 0);
    CHECK_INT(nd_addr_to_sockaddr(&a, &ss, &salen), 0);
    CHECK_INT(salen, (int)sizeof(struct sockaddr_in));
    memset(&back, 0, sizeof(back));
    CHECK_INT(nd_addr_from_sockaddr(&back, (const struct sockaddr *)&ss, salen), 0);
    CHECK_INT(back.family, AF_INET);
    CHECK_INT(back.port, 1234);
    CHECK(memcmp(back.raw, a.raw, 4) == 0);
    CHECK(nd_addr_equal(&a, &back));

    memset(&a, 0, sizeof(a));
    CHECK_INT(nd_addr_parse("[2001:db8::7]:5353", 0, &a), 0);
    CHECK_INT(nd_addr_to_sockaddr(&a, &ss, &salen), 0);
    CHECK_INT(salen, (int)sizeof(struct sockaddr_in6));
    memset(&back, 0, sizeof(back));
    CHECK_INT(nd_addr_from_sockaddr(&back, (const struct sockaddr *)&ss, salen), 0);
    CHECK_INT(back.family, AF_INET6);
    CHECK_INT(back.port, 5353);
    CHECK(nd_addr_equal(&a, &back));

    /* 非法输入 */
    CHECK(nd_addr_from_sockaddr(&back, NULL, 0) != 0);

    END_CASE();
}

static void case_mapped(void)
{
    nd_addr m;
    struct sockaddr_in6 s6;
    char out[96];

    BEGIN_CASE("IPv4 映射地址归一化");

    /* ::ffff:192.168.1.9 */
    memset(&s6, 0, sizeof(s6));
    s6.sin6_family = AF_INET6;
    s6.sin6_port = htons(8080);
    s6.sin6_addr.s6_addr[10] = 0xff;
    s6.sin6_addr.s6_addr[11] = 0xff;
    s6.sin6_addr.s6_addr[12] = 192;
    s6.sin6_addr.s6_addr[13] = 168;
    s6.sin6_addr.s6_addr[14] = 1;
    s6.sin6_addr.s6_addr[15] = 9;

    memset(&m, 0, sizeof(m));
    CHECK_INT(nd_addr_from_sockaddr(&m, (const struct sockaddr *)&s6, (int)sizeof(s6)), 0);
    CHECK_INT(m.family, AF_INET6);
    CHECK_INT(m.is_mapped, 1);

    /* 格式化时按 IPv4 展示 */
    CHECK_INT(nd_addr_format(&m, 1, out, sizeof(out)), 0);
    CHECK_STR(out, "192.168.1.9:8080");

    nd_addr_normalize(&m);
    CHECK_INT(m.family, AF_INET);
    CHECK_INT(m.is_mapped, 0);
    CHECK_INT(m.port, 8080);
    CHECK_INT(m.raw[0], 192);
    CHECK_INT(m.raw[3], 9);

    END_CASE();
}

static void case_resolve_literal(void)
{
    nd_addr out[4];
    size_t n = 0;
    char err[128];

    BEGIN_CASE("字面地址解析忽略地址族");

    /* 填了 IPv6，即使地址族选 IPv4 也按目标本身解析 */
    CHECK_INT(nd_resolve("::1", 443, ND_FAM_V4, out, 4, &n, err, sizeof(err)), 0);
    CHECK_INT(n, 1);
    CHECK_INT(out[0].family, AF_INET6);
    CHECK_INT(out[0].port, 443);

    /* 反向同理 */
    CHECK_INT(nd_resolve("127.0.0.1", 8080, ND_FAM_V6, out, 4, &n, err, sizeof(err)), 0);
    CHECK_INT(n, 1);
    CHECK_INT(out[0].family, AF_INET);
    CHECK_INT(out[0].port, 8080);

    /* 带方括号与 scope 的写法 */
    CHECK_INT(nd_resolve("[::1]", 80, ND_FAM_DUAL, out, 4, &n, err, sizeof(err)), 0);
    CHECK_INT(out[0].family, AF_INET6);
    CHECK_INT(nd_resolve("fe80::1%7", 0, ND_FAM_V4, out, 4, &n, err, sizeof(err)), 0);
    CHECK_INT(out[0].scope_id, 7);

    /* 空主机名仍然报错 */
    CHECK(nd_resolve("", 80, ND_FAM_DUAL, out, 4, &n, err, sizeof(err)) != 0);
    CHECK_INT(n, 0);

    END_CASE();
}

void suite_net_addr(void)
{
    SUITE("net.addr");
    case_parse_v4();
    case_parse_v6();
    case_parse_invalid();
    case_equal();
    case_classify();
    case_sockaddr_roundtrip();
    case_mapped();
    case_resolve_literal();
}
