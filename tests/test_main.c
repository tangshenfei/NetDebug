#include "test_framework.h"

#include "core/thread.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

nd_case_result g_cases[ND_MAX_CASES];
int g_case_count;
int g_total_checks;
int g_total_failures;

static char g_suite[48] = "";
static char g_case[96] = "";
static int g_case_checks;
static int g_case_failures;
static uint64_t g_case_start;
static int g_case_active;

static const char *short_file(const char *path)
{
    const char *p = path;
    const char *s;
    for (s = path; *s; s++) {
        if (*s == '/' || *s == '\\')
            p = s + 1;
    }
    return p;
}

void nd_tf_suite(const char *name)
{
    snprintf(g_suite, sizeof(g_suite), "%s", name);
    printf("\n[suite] %s\n", name);
}

void nd_tf_case_begin(const char *name)
{
    snprintf(g_case, sizeof(g_case), "%s", name);
    g_case_checks = 0;
    g_case_failures = 0;
    g_case_start = nd_monotonic_ms();
    g_case_active = 1;
}

static void record_failure(const char *detail)
{
    nd_case_result *c;

    g_total_failures++;
    g_case_failures++;
    if (g_case_count >= ND_MAX_CASES)
        return;
    c = &g_cases[g_case_count];
    if (c->first_failure[0] == '\0')
        snprintf(c->first_failure, sizeof(c->first_failure), "%s", detail);
}

void nd_tf_check(int ok, const char *expr, const char *file, int line)
{
    g_total_checks++;
    g_case_checks++;
    if (!ok) {
        char detail[192];
        snprintf(detail, sizeof(detail), "%s (%s:%d)", expr,
                 short_file(file), line);
        printf("       FAIL %s\n", detail);
        record_failure(detail);
    }
}

void nd_tf_check_str(const char *actual, const char *expect, const char *what,
                     const char *file, int line)
{
    g_total_checks++;
    g_case_checks++;
    if (!actual || !expect || strcmp(actual, expect) != 0) {
        char detail[192];
        snprintf(detail, sizeof(detail), "%s == \"%s\" (实际 \"%s\") (%s:%d)",
                 what, expect ? expect : "(null)",
                 actual ? actual : "(null)", short_file(file), line);
        printf("       FAIL %s\n", detail);
        record_failure(detail);
    }
}

void nd_tf_check_int(long long actual, long long expect, const char *what,
                     const char *file, int line)
{
    g_total_checks++;
    g_case_checks++;
    if (actual != expect) {
        char detail[192];
        snprintf(detail, sizeof(detail), "%s == %lld (实际 %lld) (%s:%d)",
                 what, expect, actual, short_file(file), line);
        printf("       FAIL %s\n", detail);
        record_failure(detail);
    }
}

void nd_tf_case_end(void)
{
    int ms;

    if (!g_case_active)
        return;
    g_case_active = 0;
    ms = (int)(nd_monotonic_ms() - g_case_start);

    if (g_case_count < ND_MAX_CASES) {
        nd_case_result *c = &g_cases[g_case_count];
        memset(c, 0, sizeof(*c));
        snprintf(c->suite, sizeof(c->suite), "%s", g_suite);
        snprintf(c->name, sizeof(c->name), "%s", g_case);
        c->checks = g_case_checks;
        c->failures = g_case_failures;
        c->ms = ms;
        g_case_count++;
    }
    if (g_case_failures)
        printf("  [FAIL] %-34s %d/%d 断言\n", g_case, g_case_checks - g_case_failures,
               g_case_checks);
    else
        printf("  [ ok ] %-34s %d 断言\n", g_case, g_case_checks);
}

/* ---------------- 报告输出 ---------------- */

static void xml_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    while (*in && o + 6 < cap) {
        const char *rep = NULL;
        switch (*in) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            memcpy(out + o, rep, n);
            o += n;
        } else {
            out[o++] = *in;
        }
        in++;
    }
    out[o] = '\0';
}

static const char *platform_name(void)
{
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

static void write_junit(const char *path)
{
    FILE *f = fopen(path, "w");
    int used[ND_MAX_CASES];
    int suites = 0;
    int i;

    if (!f) {
        printf("cannot write %s\n", path);
        return;
    }
    for (i = 0; i < g_case_count; i++)
        used[i] = 0;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<testsuites name=\"netdebug\" tests=\"%d\" failures=\"%d\">\n",
            g_case_count, g_total_failures);

    for (i = 0; i < g_case_count; i++) {
        char name[128];
        int j, tests = 0, fails = 0;

        if (used[i])
            continue;
        used[i] = 1;
        for (j = i; j < g_case_count; j++) {
            if (!strcmp(g_cases[j].suite, g_cases[i].suite)) {
                used[j] = 1;
                tests++;
                fails += g_cases[j].failures ? 1 : 0;
            }
        }
        xml_escape(g_cases[i].suite, name, sizeof(name));
        fprintf(f, "  <testsuite name=\"%s\" tests=\"%d\" failures=\"%d\">\n",
                name, tests, fails);
        for (j = 0; j < g_case_count; j++) {
            char cname[160], msg[256];
            if (strcmp(g_cases[j].suite, g_cases[i].suite))
                continue;
            xml_escape(g_cases[j].name, cname, sizeof(cname));
            fprintf(f,
                    "    <testcase classname=\"%s\" name=\"%s\" time=\"%.3f\">\n",
                    name, cname, (double)g_cases[j].ms / 1000.0);
            if (g_cases[j].failures) {
                xml_escape(g_cases[j].first_failure, msg, sizeof(msg));
                fprintf(f, "      <failure message=\"%s\"/>\n", msg);
            }
            fprintf(f, "    </testcase>\n");
        }
        fprintf(f, "  </testsuite>\n");
        suites++;
    }
    fprintf(f, "</testsuites>\n");
    fclose(f);
    printf("JUnit XML: %s (%d suites)\n", path, suites);
}

static void write_markdown(const char *path)
{
    FILE *f = fopen(path, "w");
    time_t now = time(NULL);
    struct tm tmv;
    char stamp[64];
    int i, cases_failed = 0;

    if (!f) {
        printf("cannot write %s\n", path);
        return;
    }
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);

    for (i = 0; i < g_case_count; i++) {
        if (g_cases[i].failures)
            cases_failed++;
    }

    fprintf(f, "# NetDebug 单元测试报告\n\n");
    fprintf(f, "| 项目 | 值 |\n|---|---|\n");
    fprintf(f, "| 生成时间 | %s |\n", stamp);
    fprintf(f, "| 平台 | %s |\n", platform_name());
    fprintf(f, "| 测试用例 | %d |\n", g_case_count);
    fprintf(f, "| 断言总数 | %d |\n", g_total_checks);
    fprintf(f, "| 失败断言 | %d |\n", g_total_failures);
    fprintf(f, "| 失败用例 | %d |\n", cases_failed);
    fprintf(f, "| 结论 | **%s** |\n\n", g_total_failures ? "失败" : "通过");

    fprintf(f, "## 按套件汇总\n\n");
    fprintf(f, "| 套件 | 用例数 | 断言数 | 失败 | 结果 |\n|---|---|---|---|---|\n");
    for (i = 0; i < g_case_count; i++) {
        int j, tests = 0, checks = 0, fails = 0;
        if (i > 0 && !strcmp(g_cases[i].suite, g_cases[i - 1].suite))
            continue;
        for (j = i; j < g_case_count; j++) {
            if (strcmp(g_cases[j].suite, g_cases[i].suite))
                continue;
            tests++;
            checks += g_cases[j].checks;
            fails += g_cases[j].failures;
        }
        fprintf(f, "| %s | %d | %d | %d | %s |\n", g_cases[i].suite, tests,
                checks, fails, fails ? "❌" : "✅");
    }

    fprintf(f, "\n## 用例明细\n\n");
    fprintf(f, "| 套件 | 用例 | 断言 | 结果 | 耗时 |\n|---|---|---|---|---|\n");
    for (i = 0; i < g_case_count; i++) {
        const nd_case_result *c = &g_cases[i];
        fprintf(f, "| %s | %s | %d | %s | %d ms |\n", c->suite, c->name,
                c->checks, c->failures ? "❌ 失败" : "✅ 通过", c->ms);
    }

    if (g_total_failures) {
        fprintf(f, "\n## 失败详情\n\n");
        for (i = 0; i < g_case_count; i++) {
            if (!g_cases[i].failures)
                continue;
            fprintf(f, "- **%s / %s**：%s\n", g_cases[i].suite,
                    g_cases[i].name, g_cases[i].first_failure);
        }
    }

    fclose(f);
    printf("Markdown  : %s\n", path);
}

int main(int argc, char **argv)
{
    const char *junit = NULL;
    const char *md = NULL;
    uint64_t t0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--junit") && i + 1 < argc)
            junit = argv[++i];
        else if (!strcmp(argv[i], "--md") && i + 1 < argc)
            md = argv[++i];
    }

    if (nd_platform_init() != 0) {
        printf("platform init failed\n");
        return 1;
    }
    t0 = nd_monotonic_ms();

    printf("NetDebug 单元测试\n=================\n");
    suite_core_util();
    suite_core_container();
    suite_net_addr();
    suite_net_url();
    suite_net_planner();
    suite_http_proto();
    suite_proto_telnet();

    nd_platform_cleanup();

    printf("\n------------------------------------------------\n");
    printf("用例 %d  断言 %d  失败 %d  耗时 %llu ms\n", g_case_count,
           g_total_checks, g_total_failures,
           (unsigned long long)(nd_monotonic_ms() - t0));
    printf("结果：%s\n", g_total_failures ? "失败" : "通过");

    if (junit)
        write_junit(junit);
    if (md)
        write_markdown(md);

    return g_total_failures ? 1 : 0;
}
