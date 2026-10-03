// =============================================================================
//  tests/test_framework.h —— 零依赖单元测试框架
//
//  为什么自己写而不是用 Unity / CMocka / greatest：
//   * 依赖越少，"换编译器/换平台时测试先崩"的概率越低。本框架只用 C11 标准库。
//   * 需要的功能其实很少：注册、断言、筛选、计时、彩色输出。加起来 200 行。
//   * 断言宏需要知道 vec3/mat4 这类自定义类型才能打印有意义的失败信息，
//     用第三方框架反而要写一堆 adapter。
//
//  用法：
//      static void vec3_add_works(void) {
//          vec3_t r = vec3_add(vec3_make(1,2,3), vec3_make(4,5,6));
//          EXPECT_VEC3_NEAR(r, vec3_make(5,7,9), F32_TOLERANCE);
//      }
//
//      void test_vec_register(void) {
//          test_register("vec", "vec3_add", vec3_add_works);
//      }
//
//  然后在 test_main.c 里调用 test_vec_register()。
//  刻意不做自动注册（链接器段技巧）：那类方案依赖具体工具链，
//  换编译器就静默失效（测试不再运行，但看起来"全过"）。
//  显式注册虽然多写一行，但少一次"测试根本没跑"的灾难。
// =============================================================================
#ifndef POLAIRE_TEST_FRAMEWORK_H
#define POLAIRE_TEST_FRAMEWORK_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/engine.h"

typedef void (*test_fn_t)(void);

void test_register(const char *suite, const char *name, test_fn_t fn);
void test_report_failure(const char *file, int line, const char *fmt, ...) ENGINE_PRINTF_FMT(3, 4);

// 运行所有已注册的测试。filter 非 NULL 时只运行名字包含该子串的用例。
// 返回 0 表示全部通过。
int test_run_all(const char *filter);

// 格式化辅助（失败信息用）
void test_format_vec2(char *buf, usize cap, const char *label, vec2_t v);
void test_format_vec3(char *buf, usize cap, const char *label, vec3_t v);
void test_format_vec4(char *buf, usize cap, const char *label, vec4_t v);
void test_format_quat(char *buf, usize cap, const char *label, quat_t q);
void test_format_mat4(char *buf, usize cap, const char *label, mat4_t m);

extern u32 g_test_assertions;
extern u32 g_test_failures;
extern u32 g_test_case_failures;

// ---------------------------------------------------------------- 核心断言
#define TF_CHECK(cond, ...)                                                                        \
    do {                                                                                           \
        ++g_test_assertions;                                                                       \
        if (!(cond)) {                                                                             \
            ++g_test_failures;                                                                     \
            ++g_test_case_failures;                                                                \
            test_report_failure(__FILE__, __LINE__, __VA_ARGS__);                                   \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------- 基础断言
#define EXPECT_TRUE(expr) TF_CHECK((expr), "EXPECT_TRUE(%s) 失败", #expr)
#define EXPECT_FALSE(expr) TF_CHECK(!(expr), "EXPECT_FALSE(%s) 失败", #expr)

#define EXPECT_NULL(ptr)                                                                           \
    TF_CHECK((ptr) == NULL, "EXPECT_NULL(%s) 失败，实际值 %p", #ptr, (const void *)(ptr))
#define EXPECT_NOT_NULL(ptr) TF_CHECK((ptr) != NULL, "EXPECT_NOT_NULL(%s) 失败", #ptr)

#define EXPECT_EQ_U64(a, b)                                                                        \
    TF_CHECK((u64)(a) == (u64)(b), "EXPECT_EQ_U64(%s, %s) 失败: %llu != %llu", #a, #b,             \
             (unsigned long long)(u64)(a), (unsigned long long)(u64)(b))

#define EXPECT_EQ_I64(a, b)                                                                        \
    TF_CHECK((i64)(a) == (i64)(b), "EXPECT_EQ_I64(%s, %s) 失败: %lld != %lld", #a, #b,             \
             (long long)(i64)(a), (long long)(i64)(b))

#define EXPECT_GT_U64(a, b)                                                                        \
    TF_CHECK((u64)(a) > (u64)(b), "EXPECT_GT_U64(%s, %s) 失败: %llu <= %llu", #a, #b,              \
             (unsigned long long)(u64)(a), (unsigned long long)(u64)(b))

#define EXPECT_LT_U64(a, b)                                                                        \
    TF_CHECK((u64)(a) < (u64)(b), "EXPECT_LT_U64(%s, %s) 失败: %llu >= %llu", #a, #b,              \
             (unsigned long long)(u64)(a), (unsigned long long)(u64)(b))

#define EXPECT_GE_U64(a, b)                                                                        \
    TF_CHECK((u64)(a) >= (u64)(b), "EXPECT_GE_U64(%s, %s) 失败: %llu < %llu", #a, #b,              \
             (unsigned long long)(u64)(a), (unsigned long long)(u64)(b))

#define EXPECT_LE_U64(a, b)                                                                        \
    TF_CHECK((u64)(a) <= (u64)(b), "EXPECT_LE_U64(%s, %s) 失败: %llu > %llu", #a, #b,              \
             (unsigned long long)(u64)(a), (unsigned long long)(u64)(b))

// 显式转成 f64 再比较，避免 -Wdouble-promotion 报警；
// 同时把两个值和差值都打出来，省得再回去加打印。
#define EXPECT_NEAR(a, b, eps)                                                                     \
    TF_CHECK(fabs((f64)(a) - (f64)(b)) <= (f64)(eps),                                              \
             "EXPECT_NEAR(%s, %s, %s) 失败: %.9g vs %.9g (差 %.3g > 容差 %.3g)", #a, #b, #eps,     \
             (f64)(a), (f64)(b), fabs((f64)(a) - (f64)(b)), (f64)(eps))

#define EXPECT_STR_EQ(a, b)                                                                        \
    TF_CHECK(strcmp((a), (b)) == 0, "EXPECT_STR_EQ(%s, %s) 失败: \"%s\" != \"%s\"", #a, #b,        \
             (const char *)(a), (const char *)(b))

// ---------------------------------------------------------------- 类型化断言
#define EXPECT_VEC2_NEAR(a, b, eps)                                                                \
    do {                                                                                           \
        ++g_test_assertions;                                                                       \
        if (!vec2_near((a), (b), (eps))) {                                                         \
            char polaire_tf_buf[256];                                                             \
            test_format_vec2(polaire_tf_buf, sizeof(polaire_tf_buf), #a, (a));                   \
            test_format_vec2(polaire_tf_buf + strlen(polaire_tf_buf),                            \
                             sizeof(polaire_tf_buf) - strlen(polaire_tf_buf), #b, (b));          \
            ++g_test_failures;                                                                     \
            ++g_test_case_failures;                                                                \
            test_report_failure(__FILE__, __LINE__, "EXPECT_VEC2_NEAR 失败: %s", polaire_tf_buf); \
        }                                                                                          \
    } while (0)

#define EXPECT_VEC3_NEAR(a, b, eps)                                                                \
    do {                                                                                           \
        ++g_test_assertions;                                                                       \
        if (!vec3_near((a), (b), (eps))) {                                                         \
            char polaire_tf_buf[256];                                                             \
            test_format_vec3(polaire_tf_buf, sizeof(polaire_tf_buf), #a, (a));                   \
            test_format_vec3(polaire_tf_buf + strlen(polaire_tf_buf),                            \
                             sizeof(polaire_tf_buf) - strlen(polaire_tf_buf), #b, (b));          \
            ++g_test_failures;                                                                     \
            ++g_test_case_failures;                                                                \
            test_report_failure(__FILE__, __LINE__, "EXPECT_VEC3_NEAR 失败: %s", polaire_tf_buf); \
        }                                                                                          \
    } while (0)

#define EXPECT_VEC4_NEAR(a, b, eps)                                                                \
    do {                                                                                           \
        ++g_test_assertions;                                                                       \
        if (!vec4_near((a), (b), (eps))) {                                                         \
            char polaire_tf_buf[256];                                                             \
            test_format_vec4(polaire_tf_buf, sizeof(polaire_tf_buf), #a, (a));                   \
            test_format_vec4(polaire_tf_buf + strlen(polaire_tf_buf),                            \
                             sizeof(polaire_tf_buf) - strlen(polaire_tf_buf), #b, (b));          \
            ++g_test_failures;                                                                     \
            ++g_test_case_failures;                                                                \
            test_report_failure(__FILE__, __LINE__, "EXPECT_VEC4_NEAR 失败: %s", polaire_tf_buf); \
        }                                                                                          \
    } while (0)

#define EXPECT_QUAT_NEAR(a, b, eps)                                                                \
    do {                                                                                           \
        ++g_test_assertions;                                                                       \
        if (!quat_near((a), (b), (eps))) {                                                         \
            char polaire_tf_buf[256];                                                             \
            test_format_quat(polaire_tf_buf, sizeof(polaire_tf_buf), #a, (a));                   \
            test_format_quat(polaire_tf_buf + strlen(polaire_tf_buf),                            \
                             sizeof(polaire_tf_buf) - strlen(polaire_tf_buf), #b, (b));          \
            ++g_test_failures;                                                                     \
            ++g_test_case_failures;                                                                \
            test_report_failure(__FILE__, __LINE__, "EXPECT_QUAT_NEAR 失败: %s", polaire_tf_buf); \
        }                                                                                          \
    } while (0)

#define EXPECT_MAT4_NEAR(a, b, eps)                                                                \
    do {                                                                                           \
        ++g_test_assertions;                                                                       \
        if (!mat4_near((a), (b), (eps))) {                                                         \
            char polaire_tf_buf[512];                                                             \
            test_format_mat4(polaire_tf_buf, sizeof(polaire_tf_buf), #a, (a));                   \
            test_format_mat4(polaire_tf_buf + strlen(polaire_tf_buf),                            \
                             sizeof(polaire_tf_buf) - strlen(polaire_tf_buf), #b, (b));          \
            ++g_test_failures;                                                                     \
            ++g_test_case_failures;                                                                \
            test_report_failure(__FILE__, __LINE__, "EXPECT_MAT4_NEAR 失败: %s", polaire_tf_buf); \
        }                                                                                          \
    } while (0)

#define EXPECT_MAT3_NEAR(a, b, eps)                                                                \
    TF_CHECK(mat3_near((a), (b), (eps)), "EXPECT_MAT3_NEAR(%s, %s, %s) 失败", #a, #b, #eps)

#endif // POLAIRE_TEST_FRAMEWORK_H
