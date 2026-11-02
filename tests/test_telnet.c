#include "test_framework.h"

#include "proto/telnet.h"

#include <string.h>

#define T_IAC  255
#define T_SB   250
#define T_WILL 251
#define T_WONT 252
#define T_DO   253
#define T_DONT 254
#define T_SE   240
#define T_NOP  241
#define OPT_ECHO 1
#define OPT_SGA  3
#define OPT_NAWS 31

static void case_plain(void)
{
    nd_telnet *t = nd_telnet_new(1);
    unsigned char out[64];
    size_t n;

    BEGIN_CASE("普通数据透传");

    CHECK(t != NULL);
    if (!t) {
        END_CASE();
        return;
    }

    n = nd_telnet_feed(t, NULL, (const unsigned char *)"hello", 5, out, sizeof(out));
    CHECK_INT(n, 5);
    CHECK(memcmp(out, "hello", 5) == 0);

    /* 空输入 */
    n = nd_telnet_feed(t, NULL, (const unsigned char *)"", 0, out, sizeof(out));
    CHECK_INT(n, 0);

    nd_telnet_free(t);
    END_CASE();
}

static void case_escaped_iac(void)
{
    nd_telnet *t = nd_telnet_new(1);
    unsigned char out[64];
    size_t n;

    BEGIN_CASE("IAC IAC 转义为单个 0xFF");

    if (!t) {
        CHECK(0);
        END_CASE();
        return;
    }

    {
        const unsigned char in[] = {'a', T_IAC, T_IAC, 'b'};
        n = nd_telnet_feed(t, NULL, in, sizeof(in), out, sizeof(out));
        CHECK_INT(n, 3);
        CHECK_INT(out[0], 'a');
        CHECK_INT(out[1], 0xFF);
        CHECK_INT(out[2], 'b');
    }

    nd_telnet_free(t);
    END_CASE();
}

static void case_command_stripping(void)
{
    nd_telnet *t = nd_telnet_new(1);
    unsigned char out[64];
    size_t n;

    BEGIN_CASE("协商命令(3 字节)被剥离");

    if (!t) {
        CHECK(0);
        END_CASE();
        return;
    }

    /* a IAC WILL SGA b */
    {
        const unsigned char in[] = {'a', T_IAC, T_WILL, OPT_SGA, 'b'};
        n = nd_telnet_feed(t, NULL, in, sizeof(in), out, sizeof(out));
        CHECK_INT(n, 2);
        CHECK(memcmp(out, "ab", 2) == 0);
    }

    /* IAC DO ECHO */
    {
        const unsigned char in[] = {'c', T_IAC, T_DO, OPT_ECHO, 'd'};
        n = nd_telnet_feed(t, NULL, in, sizeof(in), out, sizeof(out));
        CHECK_INT(n, 2);
        CHECK(memcmp(out, "cd", 2) == 0);
    }

    /* IAC WONT / IAC DONT 同样被剥离 */
    {
        const unsigned char in[] = {'e', T_IAC, T_WONT, OPT_SGA, T_IAC, T_DONT, OPT_SGA, 'f'};
        n = nd_telnet_feed(t, NULL, in, sizeof(in), out, sizeof(out));
        CHECK_INT(n, 2);
        CHECK(memcmp(out, "ef", 2) == 0);
    }

    /* IAC NOP 单字节命令直接丢弃 */
    {
        const unsigned char in[] = {'g', T_IAC, T_NOP, 'h'};
        n = nd_telnet_feed(t, NULL, in, sizeof(in), out, sizeof(out));
        CHECK_INT(n, 2);
        CHECK(memcmp(out, "gh", 2) == 0);
    }

    nd_telnet_free(t);
    END_CASE();
}

static void case_subnegotiation(void)
{
    nd_telnet *t = nd_telnet_new(1);
    unsigned char out[64];
    size_t n;

    BEGIN_CASE("子协商(NAWS)整段剥离");

    if (!t) {
        CHECK(0);
        END_CASE();
        return;
    }

    /* p IAC SB NAWS 0 80 0 24 IAC SE q */
    {
        const unsigned char in[] = {'p', T_IAC, T_SB, OPT_NAWS, 0, 80, 0, 24,
                                    T_IAC, T_SE, 'q'};
        n = nd_telnet_feed(t, NULL, in, sizeof(in), out, sizeof(out));
        CHECK_INT(n, 2);
        CHECK(memcmp(out, "pq", 2) == 0);
    }

    nd_telnet_free(t);
    END_CASE();
}

static void case_split_packets(void)
{
    nd_telnet *t = nd_telnet_new(1);
    unsigned char out[64];
    size_t n;

    BEGIN_CASE("跨数据包的 IAC 状态保持");

    if (!t) {
        CHECK(0);
        END_CASE();
        return;
    }

    /* 分三次喂入一个协商序列，应被完整识别并剥离 */
    {
        const unsigned char p1[] = {'x', T_IAC};
        const unsigned char p2[] = {T_WILL};
        const unsigned char p3[] = {OPT_SGA, 'y'};

        n = nd_telnet_feed(t, NULL, p1, sizeof(p1), out, sizeof(out));
        CHECK_INT(n, 1);
        CHECK_INT(out[0], 'x');

        n = nd_telnet_feed(t, NULL, p2, sizeof(p2), out, sizeof(out));
        CHECK_INT(n, 0);

        n = nd_telnet_feed(t, NULL, p3, sizeof(p3), out, sizeof(out));
        CHECK_INT(n, 1);
        CHECK_INT(out[0], 'y');
    }

    /* 分两次喂入转义 IAC */
    {
        const unsigned char p1[] = {'a', T_IAC};
        const unsigned char p2[] = {T_IAC, 'b'};

        n = nd_telnet_feed(t, NULL, p1, sizeof(p1), out, sizeof(out));
        CHECK_INT(n, 1);

        n = nd_telnet_feed(t, NULL, p2, sizeof(p2), out, sizeof(out));
        CHECK_INT(n, 2);
        CHECK_INT(out[0], 0xFF);
        CHECK_INT(out[1], 'b');
    }

    nd_telnet_free(t);
    END_CASE();
}

static void case_server_accepts_options(void)
{
    nd_telnet *t = nd_telnet_new(1);
    unsigned char out[64];
    size_t n;

    BEGIN_CASE("服务端接收 NAWS/TTYPE 协商不产生输出");

    if (!t) {
        CHECK(0);
        END_CASE();
        return;
    }

    {
        const unsigned char in[] = {T_IAC, T_WILL, OPT_NAWS,
                                    T_IAC, T_WILL, 24 /* TTYPE */,
                                    'z'};
        n = nd_telnet_feed(t, NULL, in, sizeof(in), out, sizeof(out));
        CHECK_INT(n, 1);
        CHECK_INT(out[0], 'z');
    }

    nd_telnet_free(t);
    END_CASE();
}

void suite_proto_telnet(void)
{
    SUITE("proto.telnet");
    case_plain();
    case_escaped_iac();
    case_command_stripping();
    case_subnegotiation();
    case_split_packets();
    case_server_accepts_options();
}
