#include "test_framework.h"

#include "core/util.h"
#include "httpd/base64.h"
#include "httpd/http.h"
#include "httpd/sha1.h"

#include <string.h>

static void sha1_hex(const void *data, size_t len, char out[41])
{
    nd_sha1_ctx c;
    unsigned char digest[20];

    nd_sha1_init(&c);
    nd_sha1_update(&c, data, len);
    nd_sha1_final(&c, digest);
    nd_hex_encode(digest, sizeof(digest), out, 41, 0);
}

static void case_sha1_vectors(void)
{
    char hex[41];

    BEGIN_CASE("SHA1 标准测试向量");

    sha1_hex("", 0, hex);
    CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    sha1_hex("abc", 3, hex);
    CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK_STR(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    sha1_hex("The quick brown fox jumps over the lazy dog", 43, hex);
    CHECK_STR(hex, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");

    END_CASE();
}

static void case_sha1_incremental(void)
{
    static const char text[] = "The quick brown fox jumps over the lazy dog";
    static char block[1000];
    nd_sha1_ctx c;
    unsigned char digest[20];
    char hex[41];
    int i;

    BEGIN_CASE("SHA1 分块更新与百万字节");

    /* 逐字节喂入，结果应与一次性计算一致 */
    nd_sha1_init(&c);
    for (i = 0; i < (int)sizeof(text) - 1; i++)
        nd_sha1_update(&c, text + i, 1);
    nd_sha1_final(&c, digest);
    nd_hex_encode(digest, sizeof(digest), hex, sizeof(hex), 0);
    CHECK_STR(hex, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");

    /* 100 万个 'a' */
    memset(block, 'a', sizeof(block));
    nd_sha1_init(&c);
    for (i = 0; i < 1000; i++)
        nd_sha1_update(&c, block, sizeof(block));
    nd_sha1_final(&c, digest);
    nd_hex_encode(digest, sizeof(digest), hex, sizeof(hex), 0);
    CHECK_STR(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    END_CASE();
}

static void case_base64_encode(void)
{
    char out[64];

    BEGIN_CASE("Base64 编码");

    CHECK_INT(nd_base64_encode("", 0, out, sizeof(out)), 0);
    CHECK_STR(out, "");

    CHECK_INT(nd_base64_encode("f", 1, out, sizeof(out)), 4);
    CHECK_STR(out, "Zg==");

    CHECK_INT(nd_base64_encode("fo", 2, out, sizeof(out)), 4);
    CHECK_STR(out, "Zm8=");

    CHECK_INT(nd_base64_encode("foo", 3, out, sizeof(out)), 4);
    CHECK_STR(out, "Zm9v");

    CHECK_INT(nd_base64_encode("foob", 4, out, sizeof(out)), 8);
    CHECK_STR(out, "Zm9vYg==");

    CHECK_INT(nd_base64_encode("fooba", 5, out, sizeof(out)), 8);
    CHECK_STR(out, "Zm9vYmE=");

    CHECK_INT(nd_base64_encode("foobar", 6, out, sizeof(out)), 8);
    CHECK_STR(out, "Zm9vYmFy");

    /* 容量不足应报错，不能截断写出 */
    CHECK_INT(nd_base64_encode("foobar", 6, out, 8), -1);

    END_CASE();
}

static void case_base64_decode(void)
{
    unsigned char buf[64];
    unsigned char all[256];
    char text[512];
    int i, n;

    BEGIN_CASE("Base64 解码");

    CHECK_INT(nd_base64_decode("", buf, sizeof(buf)), 0);

    n = nd_base64_decode("Zg==", buf, sizeof(buf));
    CHECK_INT(n, 1);
    CHECK(memcmp(buf, "f", 1) == 0);

    n = nd_base64_decode("Zm8=", buf, sizeof(buf));
    CHECK_INT(n, 2);
    CHECK(memcmp(buf, "fo", 2) == 0);

    n = nd_base64_decode("Zm9vYmFy", buf, sizeof(buf));
    CHECK_INT(n, 6);
    CHECK(memcmp(buf, "foobar", 6) == 0);

    /* 非法字符 / 长度不足 4 的单字节 */
    CHECK_INT(nd_base64_decode("!!!!", buf, sizeof(buf)), -1);
    CHECK_INT(nd_base64_decode("Z", buf, sizeof(buf)), -1);

    /* 输出容量不足 */
    CHECK_INT(nd_base64_decode("Zm9vYmFy", buf, 4), -1);

    /* 二进制全字节往返（256 字节需要 256 字节输出缓冲） */
    for (i = 0; i < 256; i++)
        all[i] = (unsigned char)i;
    n = nd_base64_encode(all, sizeof(all), text, sizeof(text));
    CHECK_INT(n, 344);
    memset(all, 0, sizeof(all));
    n = nd_base64_decode(text, all, sizeof(all));
    CHECK_INT(n, 256);
    for (i = 0; i < 256; i++) {
        if (all[i] != (unsigned char)i)
            break;
    }
    CHECK_INT(i, 256);   /* 每个字节都应还原正确 */

    END_CASE();
}

static void case_url_decode(void)
{
    char s[128];

    BEGIN_CASE("URL 解码");

    strcpy(s, "%E4%B8%AD%E6%96%87");
    nd_http_url_decode(s);
    CHECK_STR(s, "\xe4\xb8\xad\xe6\x96\x87");   /* 中文 */

    strcpy(s, "a+b");
    nd_http_url_decode(s);
    CHECK_STR(s, "a b");

    strcpy(s, "no-encoding");
    nd_http_url_decode(s);
    CHECK_STR(s, "no-encoding");

    /* 非法转义保持原样 */
    strcpy(s, "%ZZ%4");
    nd_http_url_decode(s);
    CHECK_STR(s, "%ZZ%4");

    strcpy(s, "%2F");
    nd_http_url_decode(s);
    CHECK_STR(s, "/");

    END_CASE();
}

static void case_query_get(void)
{
    char v[64];

    BEGIN_CASE("查询串取值");

    CHECK_INT(nd_http_query_get("a=1&b=2", "b", v, sizeof(v)), 0);
    CHECK_STR(v, "2");

    CHECK_INT(nd_http_query_get("a=1&b=2", "a", v, sizeof(v)), 0);
    CHECK_STR(v, "1");

    CHECK_INT(nd_http_query_get("a=1&b=2", "c", v, sizeof(v)), -1);

    /* 值里含等号 */
    CHECK_INT(nd_http_query_get("q=a=b", "q", v, sizeof(v)), 0);
    CHECK_STR(v, "a=b");

    /* 同名键取第一个 */
    CHECK_INT(nd_http_query_get("a=1&a=2", "a", v, sizeof(v)), 0);
    CHECK_STR(v, "1");

    /* 自动解码 */
    CHECK_INT(nd_http_query_get("name=%E4%B8%AD%E6%96%87&x=1", "name", v, sizeof(v)), 0);
    CHECK_STR(v, "\xe4\xb8\xad\xe6\x96\x87");

    /* 前缀相同的键不能误匹配 */
    CHECK_INT(nd_http_query_get("aa=1&a=2", "a", v, sizeof(v)), 0);
    CHECK_STR(v, "2");

    /* 边界 */
    CHECK_INT(nd_http_query_get("", "a", v, sizeof(v)), -1);
    CHECK_INT(nd_http_query_get("flag", "flag", v, sizeof(v)), -1); /* 没有 = 号 */

    /* 截断到缓冲区大小 */
    CHECK_INT(nd_http_query_get("a=abcdef", "a", v, 4), 0);
    CHECK_STR(v, "abc");

    END_CASE();
}

void suite_http_proto(void)
{
    SUITE("httpd");
    case_sha1_vectors();
    case_sha1_incremental();
    case_base64_encode();
    case_base64_decode();
    case_url_decode();
    case_query_get();
}
