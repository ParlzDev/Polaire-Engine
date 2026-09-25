// =============================================================================
//  tests/test_memory.c —— 分配器 / Arena / Pool
//
//  本文件的核心目标是把"无泄漏"从口号变成可执行断言：
//  每个用例都用追踪分配器包住堆，结束时断言 live_blocks == 0。
//  这样任何一条忘记释放的路径都会立刻变成红灯，而不是等到几小时后
//  内存涨到几个 G 才发现。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

typedef struct test_object_t {
    u32 id;
    f32 value;
    vec3_t position;
    u8  payload[16];
} test_object_t;

// =============================================================================
//  追踪分配器
// =============================================================================
static void tracking_counts_allocations_and_bytes(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
    EXPECT_EQ_U64(tracker.live_blocks, 0);

    void *a = tracker.base.alloc(&tracker.base, 128, 16);
    EXPECT_NOT_NULL(a);
    EXPECT_EQ_U64(tracker.alloc_calls, 1);
    EXPECT_EQ_U64(tracker.live_blocks, 1);
    EXPECT_EQ_U64(tracker.bytes_in_use, 128);
    EXPECT_EQ_U64(tracker.peak_bytes, 128);
    EXPECT_TRUE(allocator_tracking_has_leak(&tracker));

    void *b = tracker.base.alloc(&tracker.base, 256, 16);
    EXPECT_NOT_NULL(b);
    EXPECT_EQ_U64(tracker.live_blocks, 2);
    EXPECT_EQ_U64(tracker.bytes_in_use, 384);
    EXPECT_EQ_U64(tracker.peak_bytes, 384);

    tracker.base.free(&tracker.base, a, 128);
    EXPECT_EQ_U64(tracker.live_blocks, 1);
    EXPECT_EQ_U64(tracker.bytes_in_use, 256);
    EXPECT_EQ_U64(tracker.peak_bytes, 384); // 峰值不回落

    tracker.base.free(&tracker.base, b, 256);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
    EXPECT_EQ_U64(tracker.bytes_in_use, 0);
    EXPECT_EQ_U64(tracker.total_bytes, 384);
}

static void heap_allocator_respects_alignment(void)
{
    allocator_t *heap = allocator_heap();

    const usize alignments[] = {16, 32, 64, 128};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(alignments); ++i) {
        void *p = heap->alloc(heap, 100, alignments[i]);
        EXPECT_NOT_NULL(p);
        EXPECT_EQ_U64((usize)(uintptr_t)p % alignments[i], 0u);
        // 对齐出来的内存必须真的可写
        memset(p, 0x5A, 100);
        heap->free(heap, p, 100);
    }

    // size == 0 返回 NULL 而不是崩溃
    EXPECT_NULL(heap->alloc(heap, 0, 16));
    // free(NULL) 安全
    heap->free(heap, NULL, 0);
}

// =============================================================================
//  Arena
// =============================================================================
static void arena_basic_allocations_do_not_overlap(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(4), "basic"));

    void *a = arena_alloc(&arena, 100);
    void *b = arena_alloc(&arena, 100);
    void *c = arena_alloc(&arena, 100);

    EXPECT_NOT_NULL(a);
    EXPECT_NOT_NULL(b);
    EXPECT_NOT_NULL(c);
    EXPECT_TRUE(a != b && b != c && a != c);

    // 16 字节对齐
    EXPECT_EQ_U64((usize)(uintptr_t)a % 16u, 0u);
    EXPECT_EQ_U64((usize)(uintptr_t)b % 16u, 0u);
    EXPECT_EQ_U64((usize)(uintptr_t)c % 16u, 0u);

    // 区间不重叠
    EXPECT_TRUE((uintptr_t)a + 100u <= (uintptr_t)b);
    EXPECT_TRUE((uintptr_t)b + 100u <= (uintptr_t)c);

    // 写入不互相破坏
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 100);
    memset(c, 0xCC, 100);
    bool ok = true;
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    const u8 *pc = (const u8 *)c;
    for (usize i = 0; i < 100; ++i) {
        if (pa[i] != 0xAA || pb[i] != 0xBB || pc[i] != 0xCC) {
            ok = false;
        }
    }
    EXPECT_TRUE(ok);

    EXPECT_GT_U64(arena_bytes_used(&arena), 0u);
    EXPECT_GE_U64(arena_bytes_reserved(&arena), arena_bytes_used(&arena));
    EXPECT_EQ_U64(arena_block_count(&arena), 1u);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void arena_respects_requested_alignment(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(8), "align"));

    // 先塞一些零碎数据，制造非对齐的起点
    arena_alloc(&arena, 3);

    const usize alignments[] = {16, 32, 64, 128, 256};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(alignments); ++i) {
        void *p = arena_alloc_aligned(&arena, 64, alignments[i]);
        EXPECT_NOT_NULL(p);
        EXPECT_EQ_U64((usize)(uintptr_t)p % alignments[i], 0u);
        memset(p, 0x11, 64);
    }

    // 16 字节对齐的请求也必须在非对齐起点之后依然对齐
    void *p16 = arena_alloc(&arena, 40);
    EXPECT_EQ_U64((usize)(uintptr_t)p16 % 16u, 0u);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void arena_zero_allocation_is_actually_zero(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(2), "zero"));

    // 先用脏数据占满，再 reset，确保后续拿到的"零"不是碰巧
    u8 *dirty = (u8 *)arena_alloc(&arena, 512);
    EXPECT_NOT_NULL(dirty);
    memset(dirty, 0xFF, 512);
    arena_reset(&arena);

    u8 *zeroed = (u8 *)arena_alloc_zero(&arena, 512);
    EXPECT_NOT_NULL(zeroed);

    bool all_zero = true;
    for (usize i = 0; i < 512; ++i) {
        if (zeroed[i] != 0) {
            all_zero = false;
        }
    }
    EXPECT_TRUE(all_zero);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void arena_reset_reuses_memory_without_syscalls(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 256, "reuse"));

    arena_alloc(&arena, 200);
    const usize blocks_after_warmup = arena_block_count(&arena);
    const usize calls_after_warmup = tracker.alloc_calls;

    arena_reset(&arena);
    EXPECT_EQ_U64(arena_bytes_used(&arena), 0u);

    // 反复 reset + 同样规模的分配，不应该再向系统要内存。
    // 这就是"帧内零 malloc"的实现方式。
    for (i32 frame = 0; frame < 100; ++frame) {
        arena_reset(&arena);
        for (i32 i = 0; i < 4; ++i) {
            EXPECT_NOT_NULL(arena_alloc(&arena, 40));
        }
    }

    EXPECT_EQ_U64(arena_block_count(&arena), blocks_after_warmup);
    EXPECT_EQ_U64(tracker.alloc_calls, calls_after_warmup);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void arena_grows_by_adding_blocks(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 256, "grow"));

    // 小分配塞满第一块
    for (i32 i = 0; i < 4; ++i) {
        EXPECT_NOT_NULL(arena_alloc(&arena, 64));
    }
    const usize blocks_before = arena_block_count(&arena);

    // 超出单块容量的大分配必须能成功，并且开新块
    void *big = arena_alloc(&arena, ENGINE_KB(16));
    EXPECT_NOT_NULL(big);
    EXPECT_GT_U64(arena_block_count(&arena), blocks_before);

    // 大块内容可写，且不破坏之前的数据
    memset(big, 0x7E, ENGINE_KB(16));

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void arena_mark_and_rewind(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 256, "mark"));

    u8 *permanent = (u8 *)arena_alloc(&arena, 64);
    EXPECT_NOT_NULL(permanent);
    memset(permanent, 0x11, 64);

    const arena_mark_t mark = arena_mark(&arena);
    const usize blocks_at_mark = arena_block_count(&arena);
    const usize used_at_mark = arena_bytes_used(&arena);

    // 制造一个会开新块的大分配
    EXPECT_NOT_NULL(arena_alloc(&arena, ENGINE_KB(64)));
    EXPECT_GT_U64(arena_block_count(&arena), blocks_at_mark);

    arena_rewind(&arena, mark);

    EXPECT_EQ_U64(arena_block_count(&arena), blocks_at_mark);
    EXPECT_EQ_U64(arena_bytes_used(&arena), used_at_mark);

    // mark 之前的数据必须完好
    bool intact = true;
    for (usize i = 0; i < 64; ++i) {
        if (permanent[i] != 0x11) {
            intact = false;
        }
    }
    EXPECT_TRUE(intact);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void arena_from_external_buffer(void)
{
    // 外部缓冲模式：完全跑在栈/静态区上，零系统调用。
    static _Alignas(16) u8 buffer[1024];

    arena_t arena;
    EXPECT_TRUE(arena_init_from_buffer(&arena, buffer, sizeof(buffer), "external"));

    EXPECT_EQ_U64(arena_block_count(&arena), 1u);

    void *a = arena_alloc(&arena, 100);
    EXPECT_NOT_NULL(a);
    EXPECT_TRUE((const u8 *)a >= buffer && (const u8 *)a < buffer + sizeof(buffer));
    EXPECT_EQ_U64((usize)(uintptr_t)a % 16u, 0u);

    // 耗尽后返回 NULL，绝不试图向系统要内存
    EXPECT_NULL(arena_alloc(&arena, 100000));

    // 释放不能去 free 一个不是自己分配的指针
    arena_destroy(&arena);
}

// =============================================================================
//  Pool
// =============================================================================
static void pool_alloc_get_free(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, &tracker.base, sizeof(test_object_t), 8, "objects"));

    EXPECT_EQ_U64(pool_capacity(&pool), 8u);
    EXPECT_EQ_U64(pool_count(&pool), 0u);
    EXPECT_EQ_U64(pool_slot_size(&pool), sizeof(test_object_t));

    handle_t h = pool_alloc(&pool);
    EXPECT_TRUE(handle_is_valid(h));
    EXPECT_EQ_U64(pool_count(&pool), 1u);
    EXPECT_TRUE(handle_index(h) >= 1u); // 0 号槽永久保留

    test_object_t *obj = POOL_GET_AS(&pool, h, test_object_t);
    EXPECT_NOT_NULL(obj);
    EXPECT_TRUE(pool_valid(&pool, h));

    // 新对象必须是零初始化的
    EXPECT_EQ_U64(obj->id, 0u);
    EXPECT_NEAR(obj->value, 0.0f, 0.0f);

    obj->id = 42;
    obj->value = 1.5f;
    obj->position = vec3_make(1.0f, 2.0f, 3.0f);

    EXPECT_EQ_U64(POOL_GET_AS(&pool, h, test_object_t)->id, 42u);
    EXPECT_NEAR(POOL_GET_AS(&pool, h, test_object_t)->value, 1.5f, 0.0f);

    // 槽位 16 字节对齐，让 vec4/mat4 之类能直接放进去
    EXPECT_EQ_U64((usize)(uintptr_t)obj % 16u, 0u);

    EXPECT_TRUE(pool_free(&pool, h));
    EXPECT_EQ_U64(pool_count(&pool), 0u);
    EXPECT_NULL(pool_get(&pool, h));
    EXPECT_FALSE(pool_valid(&pool, h));
    EXPECT_FALSE(pool_free(&pool, h)); // 重复释放返回 false，不崩

    pool_destroy(&pool);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void pool_generation_prevents_dangling_handle(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, &tracker.base, sizeof(test_object_t), 4, "aba"));

    handle_t first = pool_alloc(&pool);
    POOL_GET_AS(&pool, first, test_object_t)->id = 111;
    EXPECT_TRUE(pool_free(&pool, first));

    // 空闲队列是 FIFO（见 pool_t.free_tail 的注释），所以想拿回**同一个槽位**
    // 必须先把队列里其它空槽发完。这里把 4 个槽位依次发出，
    // 最后一个拿到的才可能与 first 同槽 —— 这才是 ABA 场景。
    handle_t second = HANDLE_INVALID;
    for (u32 i = 0; i < 4u; ++i) {
        handle_t h = pool_alloc(&pool);
        EXPECT_TRUE(handle_is_valid(h));
        if (handle_index(h) == handle_index(first)) {
            second = h;
        }
    }
    EXPECT_TRUE(handle_is_valid(second));
    EXPECT_TRUE(first != second);
    EXPECT_EQ_U64(handle_generation(second), (u8)(handle_generation(first) + 1u));

    // 旧句柄必须失效 —— 这正是句柄制相对裸指针的价值
    EXPECT_NULL(pool_get(&pool, first));
    EXPECT_FALSE(pool_valid(&pool, first));
    EXPECT_NOT_NULL(pool_get(&pool, second));

    // 复用后内容是干净的，不会看到上一个对象的 id
    EXPECT_EQ_U64(POOL_GET_AS(&pool, second, test_object_t)->id, 0u);

    // 释放旧句柄必须失败（代际不匹配），不能误删新对象
    EXPECT_FALSE(pool_free(&pool, first));
    EXPECT_NOT_NULL(pool_get(&pool, second));

    pool_destroy(&pool);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void pool_generation_wraps_without_breaking(void)
{
    // 同一个槽位被反复复用超过 255 次，代际回绕后仍必须保持一致。
    // 如果 pool_slot_t::generation 被误写成 u32，这里会挂。
    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, allocator_heap(), sizeof(test_object_t), 1, "wrap"));

    handle_t handle = HANDLE_INVALID;
    for (u32 i = 0; i < 600; ++i) {
        handle = pool_alloc(&pool);
        EXPECT_TRUE(handle_is_valid(handle));
        EXPECT_EQ_U64(handle_index(handle), 1u);
        EXPECT_TRUE(handle_generation(handle) >= 1u); // 永不出现 0 代际
        EXPECT_NOT_NULL(pool_get(&pool, handle));
        EXPECT_TRUE(pool_free(&pool, handle));
    }

    pool_destroy(&pool);
}

// 空闲链表必须是 FIFO（轮转发放），不是栈。
// 这条行为保证"刚释放的槽位在其它空槽发完之前不会被再次发出"，从而把
// 8 位代际的回绕周期从 256 次放大到约 256 × 空闲槽数。
// 写成 LIFO 的话：热路径上反复 alloc/free 同一个槽位，几百次之后
// 陈旧句柄会重新变成"有效"，而且指向另一个对象 —— 幽灵 bug，无法复现。
static void pool_free_list_is_fifo_round_robin(void)
{
    const u32 count = 4u;
    pool_t    pool;
    EXPECT_TRUE(pool_init(&pool, allocator_heap(), sizeof(test_object_t), count, "fifo"));

    handle_t handles[4];
    for (u32 i = 0; i < count; ++i) {
        handles[i] = pool_alloc(&pool);
        EXPECT_TRUE(handle_is_valid(handles[i]));
        EXPECT_EQ_U64(handle_index(handles[i]), i + 1u); // 0 号槽永久保留
    }
    EXPECT_TRUE(handle_equal(pool_alloc(&pool), HANDLE_INVALID)); // 池满

    // 释放前三个，空闲队列顺序应为 1 -> 2 -> 3；最后那个保持占用，
    // 这样"队列里有多个空槽"的情形才成立（只剩一个空槽时 FIFO 与 LIFO 无法区分）。
    EXPECT_TRUE(pool_free(&pool, handles[0]));
    EXPECT_TRUE(pool_free(&pool, handles[1]));
    EXPECT_TRUE(pool_free(&pool, handles[2]));

    const handle_t a = pool_alloc(&pool);
    const handle_t b = pool_alloc(&pool);
    const handle_t c = pool_alloc(&pool);
    EXPECT_EQ_U64(handle_index(a), 1u); // LIFO 会在这里给出 3
    EXPECT_EQ_U64(handle_index(b), 2u);
    EXPECT_EQ_U64(handle_index(c), 3u);

    // 再验一次"最早释放的先发"：还回 a、b 之后先发的是 a 的那个槽位。
    EXPECT_TRUE(pool_free(&pool, a));
    EXPECT_TRUE(pool_free(&pool, b));
    const handle_t delayed = pool_alloc(&pool);
    EXPECT_EQ_U64(handle_index(delayed), handle_index(a));

    pool_destroy(&pool);
}

// 单槽池是队列的退化情形，alloc/free 交替仍要正确
// （free_tail 在队列清空时没一起清掉的话，这里链表就断了）。
static void pool_single_slot_alloc_free_loop(void)
{
    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, allocator_heap(), sizeof(test_object_t), 1, "single"));

    for (u32 i = 0; i < 300u; ++i) {
        handle_t h = pool_alloc(&pool);
        EXPECT_TRUE(handle_is_valid(h));
        EXPECT_EQ_U64(pool_count(&pool), 1u);
        EXPECT_NOT_NULL(pool_get(&pool, h));
        EXPECT_TRUE(pool_free(&pool, h));
        EXPECT_EQ_U64(pool_count(&pool), 0u);
        EXPECT_NULL(pool_get(&pool, h));
    }

    pool_destroy(&pool);
}

static void pool_full_returns_invalid(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, &tracker.base, sizeof(test_object_t), 3, "full"));

    handle_t handles[3];
    for (i32 i = 0; i < 3; ++i) {
        handles[i] = pool_alloc(&pool);
        EXPECT_TRUE(handle_is_valid(handles[i]));
    }
    EXPECT_EQ_U64(pool_count(&pool), 3u);
    EXPECT_EQ_U64(pool_peak(&pool), 3u);

    // 池满：返回无效句柄而不是扩容或崩溃
    EXPECT_EQ_U64(pool_alloc(&pool), HANDLE_INVALID);

    // 释放一个之后又能分配
    EXPECT_TRUE(pool_free(&pool, handles[1]));
    handle_t reused = pool_alloc(&pool);
    EXPECT_TRUE(handle_is_valid(reused));
    EXPECT_EQ_U64(pool_count(&pool), 3u);

    pool_destroy(&pool);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void pool_iteration_visits_only_live_slots(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, &tracker.base, sizeof(test_object_t), 10, "iterate"));

    handle_t handles[10];
    for (u32 i = 0; i < 10; ++i) {
        handles[i] = pool_alloc(&pool);
        POOL_GET_AS(&pool, handles[i], test_object_t)->id = i + 1u;
    }

    // 释放偶数下标的槽位，制造空洞
    for (u32 i = 0; i < 10; i += 2) {
        EXPECT_TRUE(pool_free(&pool, handles[i]));
    }
    EXPECT_EQ_U64(pool_count(&pool), 5u);

    u32 visited = 0;
    u32 id_sum = 0;

    pool_iter_t it;
    pool_iter_begin(&it, &pool);

    handle_t handle = HANDLE_INVALID;
    void    *ptr = NULL;
    while (pool_iter_next(&it, &handle, &ptr)) {
        visited++;
        EXPECT_NOT_NULL(ptr);
        EXPECT_TRUE(pool_valid(&pool, handle));
        // 迭代器给出的句柄必须能取回同一块内存
        EXPECT_TRUE(pool_get(&pool, handle) == ptr);
        id_sum += ((test_object_t *)ptr)->id;
    }

    EXPECT_EQ_U64(visited, 5u);
    EXPECT_EQ_U64(id_sum, 2u + 4u + 6u + 8u + 10u);

    pool_destroy(&pool);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void pool_handles_invalid_input(void)
{
    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, allocator_heap(), sizeof(test_object_t), 4, "invalid"));

    EXPECT_NULL(pool_get(&pool, HANDLE_INVALID));
    EXPECT_NULL(pool_get(&pool, handle_make(0, 1)));       // 保留槽
    EXPECT_NULL(pool_get(&pool, handle_make(9999, 1)));    // 越界索引
    EXPECT_FALSE(pool_valid(&pool, HANDLE_INVALID));
    EXPECT_FALSE(pool_free(&pool, HANDLE_INVALID));
    EXPECT_FALSE(pool_free(&pool, handle_make(9999, 1)));

    pool_destroy(&pool);
}

static void pool_peak_is_tracked(void)
{
    pool_t pool;
    EXPECT_TRUE(pool_init(&pool, allocator_heap(), sizeof(test_object_t), 8, "peak"));

    handle_t handles[8];
    for (u32 i = 0; i < 8; ++i) {
        handles[i] = pool_alloc(&pool);
    }
    EXPECT_EQ_U64(pool_peak(&pool), 8u);

    for (u32 i = 0; i < 8; ++i) {
        pool_free(&pool, handles[i]);
    }
    EXPECT_EQ_U64(pool_count(&pool), 0u);
    EXPECT_EQ_U64(pool_peak(&pool), 8u); // 峰值不回落

    pool_destroy(&pool);
}

// =============================================================================
//  组合场景：模拟一帧里同时用到 arena / pool / array / hash
// =============================================================================
static void combined_usage_leaves_no_leak(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    // 永久对象池
    pool_t entities;
    EXPECT_TRUE(pool_init(&entities, &tracker.base, sizeof(test_object_t), 64, "entities"));

    // 帧临时数据 arena
    arena_t frame_arena;
    EXPECT_TRUE(arena_init(&frame_arena, &tracker.base, ENGINE_KB(16), "frame"));

    // 动态列表
    array_t draw_list;
    EXPECT_TRUE(array_init(&draw_list, &tracker.base, sizeof(handle_t), 16));

    // 名字 -> 实体 的查找表
    hash_t name_lookup;
    EXPECT_TRUE(hash_init(&name_lookup, &tracker.base, 0));

    for (i32 frame = 0; frame < 20; ++frame) {
        arena_reset(&frame_arena);
        array_clear(&draw_list);

        for (u32 i = 0; i < 10; ++i) {
            handle_t h = pool_alloc(&entities);
            EXPECT_TRUE(handle_is_valid(h));

            test_object_t *obj = POOL_GET_AS(&entities, h, test_object_t);
            obj->id = i;
            obj->position = vec3_make((f32)i, 0.0f, (f32)frame);

            ARRAY_PUSH(&draw_list, handle_t, h);
        }

        EXPECT_EQ_U64(hash_set(&name_lookup, (u64)frame, (u64)frame * 2u), 1u);

        // 帧内临时数据全部走 arena
        vec3_t *scratch = ARENA_NEW_ARRAY(&frame_arena, vec3_t, 32);
        EXPECT_NOT_NULL(scratch);
        for (u32 i = 0; i < 32; ++i) {
            scratch[i] = vec3_splat((f32)i);
        }

        // 帧末把实体全部还回池
        ARRAY_FOREACH(&draw_list, handle_t, it) {
            EXPECT_TRUE(pool_free(&entities, *it));
        }
    }

    EXPECT_EQ_U64(pool_count(&entities), 0u);
    EXPECT_EQ_U64(hash_size(&name_lookup), 20u);

    u64 value = 0;
    EXPECT_TRUE(hash_get(&name_lookup, 7, &value));
    EXPECT_EQ_U64(value, 14u);

    hash_destroy(&name_lookup);
    array_destroy(&draw_list);
    arena_destroy(&frame_arena);
    pool_destroy(&entities);

    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
    EXPECT_EQ_U64(tracker.bytes_in_use, 0u);
}

void test_memory_register(void)
{
    test_register("memory", "tracking_counts_allocations_and_bytes",
                  tracking_counts_allocations_and_bytes);
    test_register("memory", "heap_allocator_respects_alignment", heap_allocator_respects_alignment);

    test_register("memory", "arena_basic_allocations_do_not_overlap",
                  arena_basic_allocations_do_not_overlap);
    test_register("memory", "arena_respects_requested_alignment",
                  arena_respects_requested_alignment);
    test_register("memory", "arena_zero_allocation_is_actually_zero",
                  arena_zero_allocation_is_actually_zero);
    test_register("memory", "arena_reset_reuses_memory_without_syscalls",
                  arena_reset_reuses_memory_without_syscalls);
    test_register("memory", "arena_grows_by_adding_blocks", arena_grows_by_adding_blocks);
    test_register("memory", "arena_mark_and_rewind", arena_mark_and_rewind);
    test_register("memory", "arena_from_external_buffer", arena_from_external_buffer);

    test_register("memory", "pool_alloc_get_free", pool_alloc_get_free);
    test_register("memory", "pool_generation_prevents_dangling_handle",
                  pool_generation_prevents_dangling_handle);
    test_register("memory", "pool_generation_wraps_without_breaking",
                  pool_generation_wraps_without_breaking);
    test_register("memory", "pool_free_list_is_fifo_round_robin",
                  pool_free_list_is_fifo_round_robin);
    test_register("memory", "pool_single_slot_alloc_free_loop", pool_single_slot_alloc_free_loop);
    test_register("memory", "pool_full_returns_invalid", pool_full_returns_invalid);
    test_register("memory", "pool_iteration_visits_only_live_slots",
                  pool_iteration_visits_only_live_slots);
    test_register("memory", "pool_handles_invalid_input", pool_handles_invalid_input);
    test_register("memory", "pool_peak_is_tracked", pool_peak_is_tracked);

    test_register("memory", "combined_usage_leaves_no_leak", combined_usage_leaves_no_leak);
}
