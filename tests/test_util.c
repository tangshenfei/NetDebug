#include "test_framework.h"

#include "core/error.h"
#include "core/ring.h"
#include "core/util.h"

#include <stdlib.h>

static void case_strlcpy(void)
{
    char b[8];
    size_t n;

    BEGIN_CASE("strlcpy 复制与截断");

    n = nd_strlcpy(b, "abc", sizeof(b));
    CHECK_INT(n, 3);
    CHECK_STR(b, "abc");

    n = nd_strlcpy(b, "abcdefghij", sizeof(b));
    CHECK_INT(n, 10);              /* 返回源串完整长度 */
    CHECK_INT(strlen(b), 7);       /* 目标被安全截断 */
    CHECK_STR(b, "abcdefg");

    /* cap == 0：不写入，仅返回长度 */
    CHECK_INT(nd_strlcpy(NULL, "abc", 0), 3);

    /* cap == 1：只写 NUL */
    b[0] = 'x';
    b[1] = 'y';
    CHECK_INT(nd_strlcpy(b, "abc", 1), 3);
    CHECK_INT(b[0], 0);
    CHECK_INT(b[1], 'y');

    /* 空源 */
    CHECK_INT(nd_strlcpy(b, "", sizeof(b)), 0);
    CHECK_STR(b, "");

    END_CASE();
}

static void case_strdup(void)
{
    char *copy;

    BEGIN_CASE("strdup 独立副本");

    copy = nd_strdup("hello");
    CHECK(copy != NULL);
    if (copy) {
        CHECK_STR(copy, "hello");
        copy[0] = 'H'; /* 修改副本不应影响常量 */
        CHECK_STR(copy, "Hello");
        free(copy);
    }
    CHECK(nd_strdup(NULL) == NULL);

    END_CASE();
}

static void case_trim(void)
{
    char s[64];

    BEGIN_CASE("trim 去除首尾空白");

    strcpy(s, "  \t hello world \r\n ");
    nd_trim(s);
    CHECK_STR(s, "hello world");

    strcpy(s, "nochange");
    nd_trim(s);
    CHECK_STR(s, "nochange");

    strcpy(s, "   ");
    nd_trim(s);
    CHECK_STR(s, "");

    strcpy(s, "a  b\tc");
    nd_trim(s);
    CHECK_STR(s, "a  b\tc"); /* 内部空白保留 */

    END_CASE();
}

static void case_prefix_suffix(void)
{
    BEGIN_CASE("starts_with / ends_with");

    CHECK(nd_starts_with("http://example.com", "http://"));
    CHECK(!nd_starts_with("http://example.com", "https://"));
    CHECK(nd_starts_with("abc", ""));          /* 空前缀恒真 */
    CHECK(!nd_starts_with("ab", "abc"));       /* 前缀更长 */

    CHECK(nd_ends_with("readme.txt", ".txt"));
    CHECK(!nd_ends_with("readme.txt", ".md"));
    CHECK(nd_ends_with("abc", ""));            /* 空后缀恒真 */
    CHECK(nd_ends_with("abc", "abc"));
    CHECK(!nd_ends_with("ab", "abc"));         /* 后缀更长 */

    END_CASE();
}

static void case_hex(void)
{
    char out[32];
    unsigned char buf[32];

    BEGIN_CASE("hex 编解码");

    CHECK_INT(nd_hex_encode("hello", 5, out, sizeof(out), 0), 10);
    CHECK_STR(out, "68656c6c6f");
    CHECK_INT(nd_hex_encode("hello", 5, out, sizeof(out), 1), 10);
    CHECK_STR(out, "68656C6C6F");

    /* 容量边界：需要 2*len+1 */
    CHECK_INT(nd_hex_encode("hello", 5, out, 10, 0), 0);
    CHECK_INT(nd_hex_encode("hello", 5, out, 11, 0), 10);

    CHECK_INT(nd_hex_decode("68656c6c6f", buf, sizeof(buf)), 5);
    CHECK(memcmp(buf, "hello", 5) == 0);

    CHECK_INT(nd_hex_decode("68 65 6C 6C 6F", buf, sizeof(buf)), 5);
    CHECK(memcmp(buf, "hello", 5) == 0);

    CHECK_INT(nd_hex_decode("", buf, sizeof(buf)), 0);
    CHECK_INT(nd_hex_decode("6", buf, sizeof(buf)), -1);       /* 奇数长度 */
    CHECK_INT(nd_hex_decode("zz", buf, sizeof(buf)), -1);      /* 非法字符 */
    CHECK_INT(nd_hex_decode("4142", buf, 1), -1);              /* 输出不足 */

    END_CASE();
}

static void case_utf8(void)
{
    BEGIN_CASE("UTF-8 校验");

    CHECK(nd_utf8_valid("hello", 5));
    CHECK(nd_utf8_valid("", 0));
    CHECK(nd_utf8_valid("\xe4\xb8\xad\xe6\x96\x87", 6));  /* 中文 */
    CHECK(nd_utf8_valid("\xf0\x9f\x98\x80", 4));          /* 😀 */

    CHECK(!nd_utf8_valid("\x80", 1));                /* 孤立续字节 */
    CHECK(!nd_utf8_valid("\xc0\x80", 2));            /* 过长编码 */
    CHECK(!nd_utf8_valid("\xed\xa0\x80", 3));        /* 代理区 */
    CHECK(!nd_utf8_valid("\xe4\xb8", 2));            /* 截断 */
    CHECK(!nd_utf8_valid("\xf5\x80\x80\x80", 4));    /* 超出 U+10FFFF */

    END_CASE();
}

static void case_error_strings(void)
{
    BEGIN_CASE("错误码文本");

    CHECK(nd_error_str(ND_OK) != NULL && nd_error_str(ND_OK)[0] != '\0');
    CHECK(nd_error_str(ND_E_PARAM)[0] != '\0');
    CHECK(nd_error_str(ND_E_BIND_INUSE)[0] != '\0');
    CHECK(nd_error_str(ND_E_CONNECT_TIMEOUT)[0] != '\0');
    CHECK(nd_error_str(ND_E_PROTO_AUTH)[0] != '\0');
    CHECK(nd_error_str(123456)[0] != '\0'); /* 未知码也要有文本，不能返回 NULL */

    /* 不同错误码应给出不同文本 */
    CHECK(strcmp(nd_error_str(ND_E_BIND_INUSE), nd_error_str(ND_E_AUTH)) != 0);

    END_CASE();
}

static void case_random(void)
{
    unsigned char a[16], b[16];

    BEGIN_CASE("随机数");

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    CHECK_INT(nd_random_bytes(a, sizeof(a)), 0);
    CHECK_INT(nd_random_bytes(b, sizeof(b)), 0);
    CHECK(memcmp(a, b, sizeof(a)) != 0); /* 两次取到的 16 字节不应相同 */

    END_CASE();
}

void suite_core_util(void)
{
    SUITE("core.util");
    case_strlcpy();
    case_strdup();
    case_trim();
    case_prefix_suffix();
    case_hex();
    case_utf8();
    case_error_strings();
    case_random();
}
