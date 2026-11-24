#include "test_framework.h"

#include "proto/syslog_server.h"

#include <string.h>

void suite_proto_syslog_register(void);

static void suite_proto_syslog(void)
{
    nd_syslog_msg m;

    SUITE("syslog 解析");
    BEGIN_CASE("RFC3164 标准消息");
    {
        const char *raw = "<34>Oct 11 22:14:15 mymachine su[123]: 'su root' failed";
        CHECK(nd_syslog_parse(raw, strlen(raw), &m) == 0);
        CHECK_INT(m.facility, 4);
        CHECK_INT(m.severity, 2);
        CHECK_STR(m.host, "mymachine");
        CHECK_STR(m.tag, "su[123]");
        CHECK(strstr(m.body, "'su root' failed") != NULL);
        CHECK_STR(nd_syslog_severity_name(2), "crit");
        CHECK_STR(nd_syslog_facility_name(4), "auth");
    }
    END_CASE();

    BEGIN_CASE("RFC5424 结构化消息");
    {
        const char *raw = "<165>1 2003-10-11T22:14:15.003Z host app proc 47 [id1 key=\"v\"] msg text";
        CHECK(nd_syslog_parse(raw, strlen(raw), &m) == 0);
        CHECK_INT(m.facility, 20);
        CHECK_INT(m.severity, 5);
        CHECK_STR(m.version, "1");
        CHECK_STR(m.host, "host");
        CHECK_STR(m.tag, "app");
        CHECK_STR(m.body, "msg text");
    }
    END_CASE();

    BEGIN_CASE("RFC3164 一位数日期（Jan  1 双空格）");
    {
        const char *raw = "<34>Jan  1 08:13:33 host1 skapid: STAF DB hit mac:00:0c:29";
        CHECK(nd_syslog_parse(raw, strlen(raw), &m) == 0);
        CHECK_INT(m.facility, 4);
        CHECK_INT(m.severity, 2);
        CHECK_STR(m.ts, "Jan  1 08:13:33");
        CHECK_STR(m.host, "host1");
        CHECK_STR(m.tag, "skapid");
        CHECK_STR(m.body, "STAF DB hit mac:00:0c:29");

        const char *raw2 = "<13>Jan 11 22:14:15 host2 kernel: two digit day";
        CHECK(nd_syslog_parse(raw2, strlen(raw2), &m) == 0);
        CHECK_STR(m.host, "host2");
        CHECK_STR(m.tag, "kernel");
    }
    END_CASE();

    BEGIN_CASE("无 PRI / 非法 PRI 降级为原文");
    {
        const char *raw = "plain text without pri";
        CHECK(nd_syslog_parse(raw, strlen(raw), &m) == 0);
        CHECK_INT(m.severity, -1);
        CHECK(strcmp(m.body, "text without pri") == 0);

        const char *bad = "<999>too big pri";
        CHECK(nd_syslog_parse(bad, strlen(bad), &m) == 0);
        CHECK_INT(m.severity, -1);
        CHECK(strcmp(m.body, "<999>too big pri") == 0);

        const char *blank = "   ";
        CHECK(nd_syslog_parse(blank, strlen(blank), &m) == -1);
    }
    END_CASE();

    BEGIN_CASE("级别与设施表");
    {
        CHECK_STR(nd_syslog_severity_name(0), "emerg");
        CHECK_STR(nd_syslog_severity_name(7), "debug");
        CHECK_STR(nd_syslog_severity_name(8), "unknown");
        CHECK_STR(nd_syslog_facility_name(16), "local0");
        CHECK_STR(nd_syslog_facility_name(23), "local7");
    }
    END_CASE();
}

void suite_proto_syslog_register(void)
{
    suite_proto_syslog();
}
