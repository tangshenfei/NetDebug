#ifndef ND_TEST_FRAMEWORK_H
#define ND_TEST_FRAMEWORK_H

/*
 * 极简单元测试框架：
 *   SUITE("模块名");            // 开始一个套件
 *   BEGIN_CASE("用例名");       // 开始一个用例
 *   CHECK(表达式);              // 断言
 *   END_CASE();                 // 结束用例
 *
 * 断言失败不会中断用例，只记录并继续，保证一次运行能看到所有问题。
 */

#include <stddef.h>
#include <string.h>

#define ND_MAX_CASES 512

typedef struct {
    char suite[48];
    char name[96];
    char first_failure[192];
    int  checks;
    int  failures;
    int  ms;
} nd_case_result;

extern nd_case_result g_cases[ND_MAX_CASES];
extern int g_case_count;
extern int g_total_checks;
extern int g_total_failures;

void nd_tf_suite(const char *name);
void nd_tf_case_begin(const char *name);
void nd_tf_case_end(void);
void nd_tf_check(int ok, const char *expr, const char *file, int line);
void nd_tf_check_str(const char *actual, const char *expect, const char *what,
                     const char *file, int line);
void nd_tf_check_int(long long actual, long long expect, const char *what,
                     const char *file, int line);

#define SUITE(name)        nd_tf_suite(name)
#define BEGIN_CASE(name)   nd_tf_case_begin(name)
#define END_CASE()         nd_tf_case_end()
#define CHECK(cond)        nd_tf_check((cond) ? 1 : 0, #cond, __FILE__, __LINE__)
#define CHECK_STR(actual, expect) \
    nd_tf_check_str((actual), (expect), #actual, __FILE__, __LINE__)
#define CHECK_INT(actual, expect) \
    nd_tf_check_int((long long)(actual), (long long)(expect), #actual, __FILE__, __LINE__)

/* 各套件入口 */
void suite_core_util(void);
void suite_core_container(void);
void suite_net_addr(void);
void suite_net_url(void);
void suite_net_planner(void);
void suite_http_proto(void);
void suite_proto_telnet(void);

#endif /* ND_TEST_FRAMEWORK_H */
