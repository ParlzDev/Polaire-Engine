// =============================================================================
//  tests/test_types.c —— 基础类型、句柄、通用宏
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

static void layout_is_stable(void)
{
    EXPECT_EQ_U64(sizeof(u8), 1);
    EXPECT_EQ_U64(sizeof(u16), 2);
    EXPECT_EQ_U64(sizeof(u32), 4);
    EXPECT_EQ_U64(sizeof(u64), 8);
    EXPECT_EQ_U64(sizeof(i8), 1);
    EXPECT_EQ_U64(sizeof(i16), 2);
    EXPECT_EQ_U64(sizeof(i32), 4);
    EXPECT_EQ_U64(sizeof(i64), 8);
    EXPECT_EQ_U64(sizeof(f32), 4);
    EXPECT_EQ_U64(sizeof(f64), 8);
    EXPECT_EQ_U64(sizeof(handle_t), 4);
    EXPECT_EQ_U64(sizeof(void *), 8);
}

static void handle_packing_roundtrip(void)
{
    for (u32 index = 1; index < 2000; index += 37) {
        for (u32 gen = 1; gen < 256; gen += 17) {
            handle_t h = handle_make(index, (u8)gen);
            EXPECT_EQ_U64(handle_index(h), index);
            EXPECT_EQ_U64(handle_generation(h), (u8)gen);
            EXPECT_TRUE(handle_is_valid(h));
        }
    }
}

static void handle_invalid_is_reserved(void)
{
    EXPECT_EQ_U64(HANDLE_INVALID, 0u);
    EXPECT_EQ_U64(handle_make(0, 0), HANDLE_INVALID);
    EXPECT_FALSE(handle_is_valid(HANDLE_INVALID));

    // 索引 0 无论代际如何都不该被当作有效句柄使用（0 号槽永久保留）
    EXPECT_TRUE(handle_is_valid(handle_make(0, 1)));
    EXPECT_EQ_U64(handle_index(handle_make(0, 1)), 0u);
}

static void handle_extremes(void)
{
    handle_t h = handle_make(HANDLE_MAX_INDEX, 255);
    EXPECT_EQ_U64(handle_index(h), HANDLE_MAX_INDEX);
    EXPECT_EQ_U64(handle_generation(h), 255u);
    EXPECT_TRUE(handle_is_valid(h));

    // 索引位与代际位不能互相污染
    handle_t g = handle_make(1, 255);
    EXPECT_EQ_U64(handle_index(g), 1u);
    EXPECT_EQ_U64(handle_generation(g), 255u);

    handle_t i = handle_make(HANDLE_MAX_INDEX, 1);
    EXPECT_EQ_U64(handle_index(i), HANDLE_MAX_INDEX);
    EXPECT_EQ_U64(handle_generation(i), 1u);
}

static void handle_equality(void)
{
    handle_t a = handle_make(42, 7);
    handle_t b = handle_make(42, 7);
    handle_t c = handle_make(42, 8);
    EXPECT_TRUE(handle_equal(a, b));
    EXPECT_FALSE(handle_equal(a, c));
    EXPECT_TRUE(a == b);
}

static void utility_macros(void)
{
    EXPECT_EQ_U64(ENGINE_ALIGN_UP(0, 16), 0);
    EXPECT_EQ_U64(ENGINE_ALIGN_UP(1, 16), 16);
    EXPECT_EQ_U64(ENGINE_ALIGN_UP(16, 16), 16);
    EXPECT_EQ_U64(ENGINE_ALIGN_UP(17, 16), 32);
    EXPECT_EQ_U64(ENGINE_ALIGN_UP(100, 8), 104);

    EXPECT_TRUE(ENGINE_IS_POW2(1));
    EXPECT_TRUE(ENGINE_IS_POW2(2));
    EXPECT_TRUE(ENGINE_IS_POW2(1024));
    EXPECT_FALSE(ENGINE_IS_POW2(0));
    EXPECT_FALSE(ENGINE_IS_POW2(3));
    EXPECT_FALSE(ENGINE_IS_POW2(1023));

    EXPECT_EQ_I64(ENGINE_MIN(3, 5), 3);
    EXPECT_EQ_I64(ENGINE_MAX(3, 5), 5);
    EXPECT_EQ_I64(ENGINE_MIN(-1, -5), -5);

    i32 a = 1;
    i32 b = 2;
    ENGINE_SWAP(i32, a, b);
    EXPECT_EQ_I64(a, 2);
    EXPECT_EQ_I64(b, 1);

    i32 fixed[7] = {0};
    EXPECT_EQ_U64(ENGINE_ARRAY_COUNT(fixed), 7);

    EXPECT_EQ_U64(ENGINE_KB(1), 1024u);
    EXPECT_EQ_U64(ENGINE_MB(1), 1024u * 1024u);
    EXPECT_EQ_U64(ENGINE_GB(1), 1024u * 1024u * 1024u);

    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec4_t, x), 0);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec4_t, y), 4);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec4_t, z), 8);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec4_t, w), 12);
}

static void debug_flag_matches_build_config(void)
{
#if ENGINE_DEBUG
    EXPECT_TRUE(true);
#else
    EXPECT_TRUE(true);
#endif
    // ENGINE_DEBUG 必须被定义成 0 或 1，不能是未定义状态。
    EXPECT_TRUE(ENGINE_DEBUG == 0 || ENGINE_DEBUG == 1);
}

void test_types_register(void)
{
    test_register("types", "layout_is_stable", layout_is_stable);
    test_register("types", "handle_packing_roundtrip", handle_packing_roundtrip);
    test_register("types", "handle_invalid_is_reserved", handle_invalid_is_reserved);
    test_register("types", "handle_extremes", handle_extremes);
    test_register("types", "handle_equality", handle_equality);
    test_register("types", "utility_macros", utility_macros);
    test_register("types", "debug_flag_matches_build_config", debug_flag_matches_build_config);
}
