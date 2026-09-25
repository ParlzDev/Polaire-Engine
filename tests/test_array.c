// =============================================================================
//  tests/test_array.c —— 动态数组
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

typedef struct record_t {
    u32  id;
    f32  weight;
    vec4_t color;
} record_t;

static void init_and_destroy_without_leak(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    array_t arr;
    EXPECT_TRUE(array_init(&arr, &tracker.base, sizeof(i32), 0));
    EXPECT_EQ_U64(array_size(&arr), 0u);
    EXPECT_EQ_U64(array_capacity(&arr), 0u);
    EXPECT_TRUE(array_is_empty(&arr));
    EXPECT_NULL(array_data(&arr));

    array_destroy(&arr);

    // 带初始容量的版本
    array_t arr2;
    EXPECT_TRUE(array_init(&arr2, &tracker.base, sizeof(i32), 32));
    EXPECT_GE_U64(array_capacity(&arr2), 32u);
    EXPECT_EQ_U64(array_size(&arr2), 0u);
    array_destroy(&arr2);

    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void push_and_read_back(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    array_t arr;
    EXPECT_TRUE(array_init(&arr, &tracker.base, sizeof(i32), 0));

    for (i32 i = 0; i < 100; ++i) {
        ARRAY_PUSH(&arr, i32, i);
    }

    EXPECT_EQ_U64(array_size(&arr), 100u);
    EXPECT_GE_U64(array_capacity(&arr), 100u);
    EXPECT_TRUE(ENGINE_IS_POW2(array_capacity(&arr)));

    for (i32 i = 0; i < 100; ++i) {
        EXPECT_EQ_I64(ARRAY_AT(&arr, i32, i), i);
    }

    array_destroy(&arr);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void growth_preserves_existing_elements(void)
{
    array_t arr;
    EXPECT_TRUE(array_init(&arr, allocator_heap(), sizeof(i32), 2));

    // 小容量起步，强制经历多次扩容
    for (i32 i = 0; i < 1000; ++i) {
        ARRAY_PUSH(&arr, i32, i * 3);
    }

    EXPECT_EQ_U64(array_size(&arr), 1000u);
    for (i32 i = 0; i < 1000; ++i) {
        EXPECT_EQ_I64(ARRAY_AT(&arr, i32, i), i * 3);
    }

    array_destroy(&arr);
}

static void pop_and_clear(void)
{
    array_t arr;
    EXPECT_TRUE(array_init(&arr, allocator_heap(), sizeof(i32), 0));

    for (i32 i = 0; i < 10; ++i) {
        ARRAY_PUSH(&arr, i32, i);
    }

    EXPECT_EQ_I64(ARRAY_POP(&arr, i32), 9);
    array_pop_uninit(&arr);
    EXPECT_EQ_U64(array_size(&arr), 9u);

    // 空数组 pop 必须安全
    array_clear(&arr);
    EXPECT_EQ_U64(array_size(&arr), 0u);
    array_pop_uninit(&arr);
    EXPECT_EQ_U64(array_size(&arr), 0u);

    // 清空后容量保留，可以继续用
    ARRAY_PUSH(&arr, i32, 77);
    EXPECT_EQ_I64(ARRAY_AT(&arr, i32, 0), 77);

    array_destroy(&arr);
}

static void resize_zero_fills_new_elements(void)
{
    array_t arr;
    EXPECT_TRUE(array_init(&arr, allocator_heap(), sizeof(i32), 4));

    for (i32 i = 0; i < 4; ++i) {
        ARRAY_PUSH(&arr, i32, 100 + i);
    }

    EXPECT_TRUE(array_resize(&arr, 16));
    EXPECT_EQ_U64(array_size(&arr), 16u);

    // 旧元素保留
    for (i32 i = 0; i < 4; ++i) {
        EXPECT_EQ_I64(ARRAY_AT(&arr, i32, i), 100 + i);
    }
    // 新元素为零
    for (u32 i = 4; i < 16; ++i) {
        EXPECT_EQ_I64(ARRAY_AT(&arr, i32, i), 0);
    }

    // 缩小只改 size，不释放内存
    const u32 capacity_before = array_capacity(&arr);
    EXPECT_TRUE(array_resize(&arr, 2));
    EXPECT_EQ_U64(array_size(&arr), 2u);
    EXPECT_EQ_U64(array_capacity(&arr), capacity_before);

    array_destroy(&arr);
}

static void reserve_never_shrinks(void)
{
    array_t arr;
    EXPECT_TRUE(array_init(&arr, allocator_heap(), sizeof(i32), 0));

    EXPECT_TRUE(array_reserve(&arr, 100));
    const u32 capacity = array_capacity(&arr);
    EXPECT_GE_U64(capacity, 100u);

    // 请求更小的容量不应导致重新分配
    EXPECT_TRUE(array_reserve(&arr, 10));
    EXPECT_EQ_U64(array_capacity(&arr), capacity);

    // 容量只增不减
    EXPECT_TRUE(array_reserve(&arr, capacity + 1));
    EXPECT_GT_U64(array_capacity(&arr), capacity);

    array_destroy(&arr);
}

static void element_alignment_supports_vec4(void)
{
    array_t arr;
    EXPECT_TRUE(array_init(&arr, allocator_heap(), sizeof(vec4_t), 4));

    for (i32 i = 0; i < 50; ++i) {
        ARRAY_PUSH(&arr, vec4_t, vec4_make((f32)i, 0.0f, 0.0f, 1.0f));
    }

    // 每个元素都必须 16 字节对齐，否则将来做 SIMD 会崩
    ARRAY_FOREACH(&arr, vec4_t, it) {
        EXPECT_EQ_U64((usize)(uintptr_t)it % 16u, 0u);
    }

    array_destroy(&arr);
}

static void struct_elements_are_copied_by_value(void)
{
    array_t arr;
    EXPECT_TRUE(array_init(&arr, allocator_heap(), sizeof(record_t), 0));

    for (u32 i = 0; i < 20; ++i) {
        record_t r;
        r.id = i;
        r.weight = (f32)i * 0.5f;
        r.color = vec4_make((f32)i, 0.0f, 0.0f, 1.0f);
        ARRAY_PUSH(&arr, record_t, r);
    }

    EXPECT_EQ_U64(array_size(&arr), 20u);
    for (u32 i = 0; i < 20; ++i) {
        record_t *r = &ARRAY_AT(&arr, record_t, i);
        EXPECT_EQ_U64(r->id, i);
        EXPECT_NEAR(r->weight, (f32)i * 0.5f, 1e-6f);
        EXPECT_NEAR(r->color.x, (f32)i, 1e-6f);
    }

    // 通过指针修改元素必须生效（不是副本）
    ARRAY_AT(&arr, record_t, 3).id = 999;
    EXPECT_EQ_U64(ARRAY_AT(&arr, record_t, 3).id, 999u);

    array_destroy(&arr);
}

static void foreach_visits_all_elements_in_order(void)
{
    array_t arr;
    EXPECT_TRUE(array_init(&arr, allocator_heap(), sizeof(i32), 0));

    for (i32 i = 0; i < 50; ++i) {
        ARRAY_PUSH(&arr, i32, i);
    }

    i64 sum = 0;
    i32 expected_index = 0;
    ARRAY_FOREACH(&arr, i32, it) {
        EXPECT_EQ_I64(*it, expected_index);
        sum += *it;
        expected_index++;
    }
    EXPECT_EQ_U64(sum, 50u * 49u / 2u);
    EXPECT_EQ_I64(expected_index, 50);

    array_destroy(&arr);
}

static void destroy_resets_to_zero_state(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    array_t arr;
    EXPECT_TRUE(array_init(&arr, &tracker.base, sizeof(i32), 8));
    ARRAY_PUSH(&arr, i32, 1);

    array_destroy(&arr);

    EXPECT_NULL(arr.data);
    EXPECT_EQ_U64(arr.size, 0u);
    EXPECT_EQ_U64(arr.capacity, 0u);
    EXPECT_NULL(arr.alloc);

    // 重复 destroy 必须安全
    array_destroy(&arr);

    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

void test_array_register(void)
{
    test_register("array", "init_and_destroy_without_leak", init_and_destroy_without_leak);
    test_register("array", "push_and_read_back", push_and_read_back);
    test_register("array", "growth_preserves_existing_elements", growth_preserves_existing_elements);
    test_register("array", "pop_and_clear", pop_and_clear);
    test_register("array", "resize_zero_fills_new_elements", resize_zero_fills_new_elements);
    test_register("array", "reserve_never_shrinks", reserve_never_shrinks);
    test_register("array", "element_alignment_supports_vec4", element_alignment_supports_vec4);
    test_register("array", "struct_elements_are_copied_by_value", struct_elements_are_copied_by_value);
    test_register("array", "foreach_visits_all_elements_in_order", foreach_visits_all_elements_in_order);
    test_register("array", "destroy_resets_to_zero_state", destroy_resets_to_zero_state);
}
