// =============================================================================
//  tests/test_framework.c —— 测试运行器实现
// =============================================================================
#include "test_framework.h"

#include <stdarg.h>
#include <time.h>

#include "engine/core/console.h"

#define TF_MAX_TESTS 1024

typedef struct test_entry_t {
    const char *suite;
    const char *name;
    test_fn_t   fn;
} test_entry_t;

static test_entry_t s_tests[TF_MAX_TESTS];
static u32          s_test_count = 0;

u32 g_test_assertions     = 0;
u32 g_test_failures       = 0;
u32 g_test_case_failures  = 0;

// 记录失败用例，最后统一列出。混在运行日志里会很难一眼看出"到底挂了几个"。
static const char *s_failed_names[TF_MAX_TESTS];
static u32         s_failed_count = 0;

void test_register(const char *suite, const char *name, test_fn_t fn)
{
    if (s_test_count >= TF_MAX_TESTS) {
        fprintf(stderr, "测试框架：注册数量超过上限 %d\n", TF_MAX_TESTS);
        return;
    }
    s_tests[s_test_count].suite = suite;
    s_tests[s_test_count].name  = name;
    s_tests[s_test_count].fn    = fn;
    s_test_count++;
}

void test_report_failure(const char *file, int line, const char *fmt, ...)
{
    char message[1024];

    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    printf("        位置: %s:%d\n", file, line);
    printf("        原因: %s\n", message);
}

// ---------------------------------------------------------------- 格式化辅助
void test_format_vec2(char *buf, usize cap, const char *label, vec2_t v)
{
    snprintf(buf, cap, "\n        %s = (%.6g, %.6g)", label, (f64)v.x, (f64)v.y);
}

void test_format_vec3(char *buf, usize cap, const char *label, vec3_t v)
{
    snprintf(buf, cap, "\n        %s = (%.6g, %.6g, %.6g)", label, (f64)v.x, (f64)v.y, (f64)v.z);
}

void test_format_vec4(char *buf, usize cap, const char *label, vec4_t v)
{
    snprintf(buf, cap, "\n        %s = (%.6g, %.6g, %.6g, %.6g)", label, (f64)v.x, (f64)v.y, (f64)v.z,
             (f64)v.w);
}

void test_format_quat(char *buf, usize cap, const char *label, quat_t q)
{
    snprintf(buf, cap, "\n        %s = (x=%.6g, y=%.6g, z=%.6g, w=%.6g)", label, (f64)q.x, (f64)q.y,
             (f64)q.z, (f64)q.w);
}

void test_format_mat4(char *buf, usize cap, const char *label, mat4_t m)
{
    snprintf(buf, cap,
             "\n        %s =\n"
             "            [ %9.5f %9.5f %9.5f %9.5f ]\n"
             "            [ %9.5f %9.5f %9.5f %9.5f ]\n"
             "            [ %9.5f %9.5f %9.5f %9.5f ]\n"
             "            [ %9.5f %9.5f %9.5f %9.5f ]",
             label, (f64)m.col[0].x, (f64)m.col[1].x, (f64)m.col[2].x, (f64)m.col[3].x,
             (f64)m.col[0].y, (f64)m.col[1].y, (f64)m.col[2].y, (f64)m.col[3].y,
             (f64)m.col[0].z, (f64)m.col[1].z, (f64)m.col[2].z, (f64)m.col[3].z,
             (f64)m.col[0].w, (f64)m.col[1].w, (f64)m.col[2].w, (f64)m.col[3].w);
}

// ---------------------------------------------------------------- 计时
static f64 tf_now_ms(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (f64)ts.tv_sec * 1000.0 + (f64)ts.tv_nsec / 1000000.0;
}

// ---------------------------------------------------------------- 运行器
int test_run_all(const char *filter)
{
    const bool ansi = console_enable_ansi();

    const char *c_reset = ansi ? "\x1b[0m" : "";
    const char *c_dim   = ansi ? "\x1b[90m" : "";
    const char *c_green = ansi ? "\x1b[32m" : "";
    const char *c_red   = ansi ? "\x1b[31m" : "";
    const char *c_bold  = ansi ? "\x1b[1m" : "";

    printf("%s========== Polaire Engine 单元测试 ==========%s\n", c_bold, c_reset);
    if (filter != NULL) {
        printf("%s筛选条件: \"%s\"%s\n", c_dim, filter, c_reset);
    }
    printf("\n");

    const f64 start_time = tf_now_ms();

    u32 ran = 0;

    for (u32 i = 0; i < s_test_count; ++i) {
        const test_entry_t *entry = &s_tests[i];

        char full_name[256];
        snprintf(full_name, sizeof(full_name), "%s/%s", entry->suite, entry->name);

        if (filter != NULL && strstr(full_name, filter) == NULL) {
            continue;
        }

        printf("%s[ RUN      ]%s %s\n", c_dim, c_reset, full_name);

        const u32 failures_before = g_test_failures;
        const f64 case_start      = tf_now_ms();

        g_test_case_failures = 0;
        entry->fn();

        const f64 elapsed = tf_now_ms() - case_start;
        ran++;

        if (g_test_failures > failures_before) {
            printf("%s[  FAILED  ]%s %s %s(%.3f ms)%s\n", c_red, c_reset, full_name, c_dim, elapsed,
                   c_reset);
            if (s_failed_count < TF_MAX_TESTS) {
                // 名字存进静态数组，所以需要一个稳定的字符串。
                // 用栈上的 full_name 会悬空，这里重新生成一份。
                static char failed_storage[TF_MAX_TESTS][256];
                snprintf(failed_storage[s_failed_count], sizeof(failed_storage[0]), "%s", full_name);
                s_failed_names[s_failed_count] = failed_storage[s_failed_count];
                s_failed_count++;
            }
        } else {
            printf("%s[       OK ]%s %s %s(%.3f ms)%s\n", c_green, c_reset, full_name, c_dim, elapsed,
                   c_reset);
        }
    }

    const f64 total_elapsed = tf_now_ms() - start_time;

    printf("\n%s========== 汇总 ==========%s\n", c_bold, c_reset);
    printf("  运行用例    : %u\n", ran);
    printf("  断言次数    : %u\n", g_test_assertions);
    printf("  失败断言    : %u\n", g_test_failures);
    printf("  总耗时      : %.3f ms\n", total_elapsed);

    if (g_test_failures == 0) {
        printf("\n%s[  PASSED  ] 全部通过%s\n", c_green, c_reset);
        return 0;
    }

    printf("\n%s[  FAILED  ] %u 个用例失败:%s\n", c_red, s_failed_count, c_reset);
    for (u32 i = 0; i < s_failed_count; ++i) {
        printf("%s  - %s%s\n", c_red, s_failed_names[i], c_reset);
    }
    return 1;
}
