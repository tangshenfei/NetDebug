#include "test_framework.h"

#include "net/url.h"

static char g_err[256];

static void case_parse_basic(void)
{
    nd_url u;

    BEGIN_CASE("URL 基本解析");

    CHECK_INT(nd_url_parse("http://example.com/path", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.scheme, "http");
    CHECK_STR(u.host, "example.com");
    CHECK_INT(u.port, 80);
    CHECK_STR(u.path, "/path");
    CHECK_INT(u.tls, 0);

    CHECK_INT(nd_url_parse("https://example.com", &u, g_err, sizeof(g_err)), 0);
    CHECK_INT(u.port, 443);
    CHECK_INT(u.tls, 1);
    CHECK_STR(u.path, "/");          /* 缺省路径 */

    /* 协议名大小写不敏感 */
    CHECK_INT(nd_url_parse("HTTP://Example.COM:8080/a?b=c", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.scheme, "http");
    CHECK_STR(u.host, "Example.COM");
    CHECK_INT(u.port, 8080);
    CHECK_STR(u.path, "/a?b=c");

    END_CASE();
}

static void case_parse_paths(void)
{
    nd_url u;

    BEGIN_CASE("URL 路径/查询/片段处理");

    /* fragment 被去掉 */
    CHECK_INT(nd_url_parse("http://127.0.0.1:18080/get#frag", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.path, "/get");
    CHECK_INT(u.port, 18080);

    /* 只有查询串时补 "/" */
    CHECK_INT(nd_url_parse("http://h?q=1", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.path, "/?q=1");

    /* 只有 fragment */
    CHECK_INT(nd_url_parse("http://h#x", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.path, "/");

    /* IPv6 主机 */
    CHECK_INT(nd_url_parse("http://[::1]:9000/x", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.host, "::1");
    CHECK_INT(u.port, 9000);
    CHECK_STR(u.path, "/x");

    END_CASE();
}

static void case_parse_invalid(void)
{
    nd_url u;

    BEGIN_CASE("URL 非法输入");

    CHECK(nd_url_parse("", &u, g_err, sizeof(g_err)) != 0);
    CHECK(nd_url_parse(NULL, &u, g_err, sizeof(g_err)) != 0);
    CHECK(nd_url_parse("example.com/path", &u, g_err, sizeof(g_err)) != 0);
    CHECK(nd_url_parse("ftp://example.com", &u, g_err, sizeof(g_err)) != 0);
    CHECK(nd_url_parse("http://user@host/", &u, g_err, sizeof(g_err)) != 0);
    CHECK(nd_url_parse("http://host:0/", &u, g_err, sizeof(g_err)) != 0);
    CHECK(nd_url_parse("http://host:70000/", &u, g_err, sizeof(g_err)) != 0);
    CHECK(nd_url_parse("http://[::1/", &u, g_err, sizeof(g_err)) != 0);

    END_CASE();
}

static void case_resolve(void)
{
    nd_url base, u;

    BEGIN_CASE("重定向 URL 解析");

    CHECK_INT(nd_url_parse("http://example.com/dir/page", &base, g_err, sizeof(g_err)), 0);

    /* 绝对路径 */
    CHECK_INT(nd_url_resolve(&base, "/other", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.host, "example.com");
    CHECK_INT(u.port, 80);
    CHECK_STR(u.path, "/other");

    /* 相对路径拼到当前目录 */
    CHECK_INT(nd_url_resolve(&base, "next", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.path, "/dir/next");

    /* 完整 URL 直接采用 */
    CHECK_INT(nd_url_resolve(&base, "https://cdn.example.net/a", &u, g_err, sizeof(g_err)), 0);
    CHECK_INT(u.tls, 1);
    CHECK_STR(u.host, "cdn.example.net");
    CHECK_INT(u.port, 443);
    CHECK_STR(u.path, "/a");

    /* 非默认端口要保留 */
    CHECK_INT(nd_url_parse("http://example.com:8080/dir/page", &base, g_err, sizeof(g_err)), 0);
    CHECK_INT(nd_url_resolve(&base, "/x", &u, g_err, sizeof(g_err)), 0);
    CHECK_INT(u.port, 8080);
    CHECK_STR(u.path, "/x");

    /* IPv6 主机加端口 */
    CHECK_INT(nd_url_parse("http://[::1]:8080/a/b", &base, g_err, sizeof(g_err)), 0);
    CHECK_INT(nd_url_resolve(&base, "c", &u, g_err, sizeof(g_err)), 0);
    CHECK_STR(u.host, "::1");
    CHECK_INT(u.port, 8080);
    CHECK_STR(u.path, "/a/c");

    /* 空 Location 应报错 */
    CHECK(nd_url_resolve(&base, "", &u, g_err, sizeof(g_err)) != 0);

    END_CASE();
}

void suite_net_url(void)
{
    SUITE("net.url");
    case_parse_basic();
    case_parse_paths();
    case_parse_invalid();
    case_resolve();
}
