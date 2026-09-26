// =============================================================================
//  tests/test_failure_paths.c —— 分配失败这条路
//
//  M0 的规划要求负面用例覆盖"分配失败"。这一条此前**一条都没跑过**：
//  仓库里唯一的包装分配器 `allocator_tracking_t` 只计数、不会失败，所以
//  `pool_init` / `array_reserve` 里那些 `return false` 的分支从没被执行过。
//  而这些分支的契约是"返回 false，并且不留半份状态"——
//  只有真的失败一次才看得见它是否成立；写错的症状不是当下崩，
//  而是"init 失败但已经吞掉一块内存没有归还"，那要等到跑很久以后才在
//  存活字节数上露出来，而那时候没人会怀疑到 init 的错误路径上。
//
//  失败点的选法：不是"第一次就失败"，而是"第 N 次失败"。真实的失败几乎总发生在
//  中间（元数据拿到了、第二块失败），那才是"会不会回滚第一块"的场景。
//  所以每个用例都从 N=1 扫到 N=块数，逐格验"要么全成功、要么干净地失败"。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include <string.h>

#include "engine/core/array.h"
#include "engine/core/memory.h"
#include "fake_allocator.h"

#define FP_SLOTS 8u
#define FP_ITEMS 16u

typedef struct fp_block_t {
    u32 a;
    u64 b;
} fp_block_t;

// ---------------------------------------------------------------- 池

// pool_init 里每一次分配都可能失败：逐个位置失败一次，验两件事 ——
// 返回 false，并且**没有留下还挂着的块**（这就是"干净地失败"）。
static void pool_init_fails_cleanly_at_every_allocation(void)
{
    for (usize stop = 1u; stop <= 8u; ++stop) {
        fake_allocator_t fake;
        fake_allocator_init(&fake, stop);

        pool_t pool;
        const bool ok = pool_init(&pool, &fake.base, sizeof(fp_block_t), FP_SLOTS, "fp_pool");
        if (stop == 1u) {
            EXPECT_FALSE(ok); // 第一次就失败：一定是 false
        }
        if (ok) {
            pool_destroy(&pool);
        }
        fake_allocator_drain(&fake);
        // 失败的那次之后不许还有块挂着；成功的路径上 destroy 也要收干净。
        EXPECT_EQ_U64(fake.live, 0u);
    }
}

// init 失败之后仍然可以重试成功。
//
// 为什么不断言"失败的结构体处于某种可检查状态"：那需要先给"半初始化的 pool_t
// 能不能被查询"定一条契约，而现在没人定过 —— 拿一个 init 失败的结构体去 pool_valid,
// 读到的是没被写过的内存，测的就不是契约而是巧合（真去调还可能当场崩，
// 表现是"测试崩了"而不是"用例失败"，那是最没用的失败形态）。
// 所以这里只断可断的：失败不泄漏，且换一个好分配器重来要能成。
static void pool_init_failure_can_be_retried(void)
{
    fake_allocator_t fake;
    fake_allocator_init(&fake, 1u); // 一律失败

    pool_t pool;
    EXPECT_FALSE(pool_init(&pool, &fake.base, sizeof(fp_block_t), FP_SLOTS, "fp_pool_bad"));
    EXPECT_GT_U64((usize)fake.failures, 0u);
    fake_allocator_drain(&fake);
    EXPECT_EQ_U64(fake.live, 0u);

    fake.fail_from_call = FAKE_ALLOC_NEVER; // 同一个包装器修好之后重试
    EXPECT_TRUE(pool_init(&pool, &fake.base, sizeof(fp_block_t), FP_SLOTS, "fp_pool_good"));
    const handle_t h = pool_alloc(&pool);
    EXPECT_TRUE(pool_valid(&pool, h));
    pool_destroy(&pool);
    fake_allocator_drain(&fake);
    EXPECT_EQ_U64(fake.live, 0u);
}

// ---------------------------------------------------------------- 动态数组

// array_reserve 的增长失败：数据与长度必须保持原样。
// 这一条为什么值得写：增长失败的实现最容易写成"先 free 旧块再 malloc 新块"——
// 那样失败时数据就没了，而且函数还返回 false 装作一切正常。
// 正确的做法是先分配新块、拷贝、再释放旧块；只有真的注入失败才能区分这两种写法。
static void array_growth_failure_keeps_contents(void)
{
    array_t arr;
    fake_allocator_t fake;
    fake_allocator_init(&fake, FAKE_ALLOC_NEVER);
    EXPECT_TRUE(array_init(&arr, &fake.base, sizeof(fp_block_t), 4u));

    // 先放 4 个元素（填满初始容量）
    for (u32 i = 0u; i < 4u; ++i) {
        fp_block_t *slot = (fp_block_t *)array_push_uninit(&arr);
        EXPECT_NOT_NULL(slot);
        if (slot != NULL) {
            slot->a = i + 1u;
            slot->b = (u64)(i + 1u) * 1000u;
        }
    }
    EXPECT_EQ_U64(array_size(&arr), 4u);
    // 失败之前的样子先记下来：断"失败没有改动它"必须拿失败之前的值来比，
    // 在失败之后再读一遍就是自己跟自己比 —— 那种断言永远为真，比没有更糟。
    const u32 size_before = array_size(&arr);
    const u32 capacity_before = array_capacity(&arr);

    // 现在让分配失败，再要求扩容
    fake.fail_from_call = fake.calls + 1u;
    EXPECT_FALSE(array_reserve(&arr, 64u));
    EXPECT_FALSE(array_resize(&arr, 64u));

    // 长度与内容一字不动 —— 这是这条路径的全部意义。
    // 容量按"失败前后不变"断而不是按具体数字断：初始容量会被对齐/翻倍策略改，
    // 而"失败不许动容量"才是契约，写死数字只是把实现选择钉成需求。
    EXPECT_EQ_U64((usize)array_size(&arr), (usize)size_before);
    EXPECT_EQ_U64((usize)array_capacity(&arr), (usize)capacity_before);
    for (u32 i = 0u; i < 4u; ++i) {
        const fp_block_t *item = (const fp_block_t *)array_at(&arr, i);
        EXPECT_NOT_NULL(item);
        if (item != NULL) {
            EXPECT_EQ_U64(item->a, (usize)(i + 1u));
            EXPECT_EQ_U64(item->b, (u64)(i + 1u) * 1000u);
        }
    }

    // 失败是可恢复的：接着分配成功时，数组还能正常长大
    fake.fail_from_call = FAKE_ALLOC_NEVER;
    EXPECT_TRUE(array_reserve(&arr, 8u));
    EXPECT_GT_U64((usize)array_capacity(&arr), 4u);
    EXPECT_EQ_U64(array_size(&arr), 4u); // 长大不许改长度

    array_destroy(&arr);
    fake_allocator_drain(&fake);
    EXPECT_EQ_U64(fake.live, 0u);
}

// 逐个分配位置失败：验"任何一次失败都不泄漏"，而不是只验最外层那一次。
static void array_reserve_fails_cleanly_at_every_allocation(void)
{
    for (usize stop = 1u; stop <= 12u; ++stop) {
        fake_allocator_t fake;
        fake_allocator_init(&fake, stop);

        array_t arr;
        const bool inited = array_init(&arr, &fake.base, sizeof(fp_block_t), 2u);
        if (inited) {
            (void)array_reserve(&arr, FP_ITEMS);
            (void)array_resize(&arr, 3u);
            array_destroy(&arr);
        }
        fake_allocator_drain(&fake);
        EXPECT_EQ_U64(fake.live, 0u);
    }
}

// =============================================================================
void test_failure_paths_register(void)
{
    test_register("failure", "pool_init_fails_cleanly_at_every_allocation",
                  pool_init_fails_cleanly_at_every_allocation);
    test_register("failure", "pool_init_failure_can_be_retried",
                  pool_init_failure_can_be_retried);
    test_register("failure", "array_growth_failure_keeps_contents",
                  array_growth_failure_keeps_contents);
    test_register("failure", "array_reserve_fails_cleanly_at_every_allocation",
                  array_reserve_fails_cleanly_at_every_allocation);
}
