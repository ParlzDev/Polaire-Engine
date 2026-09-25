// =============================================================================
//  src/core/memory.c —— 分配器、Arena、Pool 实现
// =============================================================================
#include "engine/core/memory.h"

#include <string.h>

#include "engine/core/assert.h"

#if defined(_MSC_VER)
#include <malloc.h> // _aligned_malloc / _aligned_free
#else
#include <stdlib.h>
#endif

// =============================================================================
//  堆分配器
// =============================================================================
static void *heap_alloc(allocator_t *self, usize size, usize alignment)
{
    ENGINE_UNUSED(self);

    if (size == 0) {
        return NULL;
    }
    if (alignment == 0) {
        alignment = 16;
    }
    ASSERT(ENGINE_IS_POW2(alignment));

#if defined(_MSC_VER)
    return _aligned_malloc(size, alignment);
#else
    // C11 的 aligned_alloc 要求 size 是 alignment 的整数倍，否则行为未定义。
    // 这里主动向上取整以满足要求；多出来的尾部空间浪费可忽略。
    // 注意：macOS 在 10.15 之前没有 aligned_alloc，届时需要换成 posix_memalign。
    return aligned_alloc(alignment, ENGINE_ALIGN_UP(size, alignment));
#endif
}

static void heap_free(allocator_t *self, void *ptr, usize size)
{
    ENGINE_UNUSED(self);
    ENGINE_UNUSED(size);

    if (ptr == NULL) {
        return;
    }
#if defined(_MSC_VER)
    // 必须与 _aligned_malloc 配对。用 free() 释放 _aligned_malloc 的内存
    // 会立刻破坏堆，且错误现场往往离调用点很远。
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static allocator_t g_heap_allocator = {
    .alloc = heap_alloc,
    .free  = heap_free,
    .ctx   = NULL,
};

allocator_t *allocator_heap(void)
{
    return &g_heap_allocator;
}

// =============================================================================
//  追踪分配器
// =============================================================================
static void *tracking_alloc(allocator_t *self, usize size, usize alignment)
{
    allocator_tracking_t *tracker = (allocator_tracking_t *)self;

    void *ptr = tracker->inner->alloc(tracker->inner, size, alignment);
    if (ptr != NULL) {
        tracker->alloc_calls++;
        tracker->live_blocks++;
        tracker->bytes_in_use += size;
        tracker->total_bytes += size;
        if (tracker->bytes_in_use > tracker->peak_bytes) {
            tracker->peak_bytes = tracker->bytes_in_use;
        }
    }
    return ptr;
}

static void tracking_free(allocator_t *self, void *ptr, usize size)
{
    allocator_tracking_t *tracker = (allocator_tracking_t *)self;

    if (ptr == NULL) {
        return;
    }

    // 多释放/重复释放是内存层的头号 bug，这里用断言把它钉在调用现场，
    // 而不是让它变成"某次分配莫名其妙失败"这种远程症状。
    ASSERT_MSG(tracker->live_blocks > 0, "追踪分配器检测到多余的 free（释放次数超过分配次数）");
    ASSERT_MSG(tracker->bytes_in_use >= size, "追踪分配器检测到释放字节数超过存活字节数");

    tracker->free_calls++;
    if (tracker->live_blocks > 0) {
        tracker->live_blocks--;
    }
    if (tracker->bytes_in_use >= size) {
        tracker->bytes_in_use -= size;
    }

    tracker->inner->free(tracker->inner, ptr, size);
}

void allocator_tracking_init(allocator_tracking_t *tracker, allocator_t *inner)
{
    ASSERT(tracker != NULL);
    ASSERT(inner != NULL);

    memset(tracker, 0, sizeof(*tracker));
    tracker->base.alloc = tracking_alloc;
    tracker->base.free  = tracking_free;
    tracker->base.ctx   = tracker;
    tracker->inner      = inner;
}

void allocator_tracking_reset(allocator_tracking_t *tracker)
{
    ASSERT(tracker != NULL);

    tracker->alloc_calls  = 0;
    tracker->free_calls   = 0;
    tracker->live_blocks  = 0;
    tracker->bytes_in_use = 0;
    tracker->peak_bytes   = 0;
    tracker->total_bytes  = 0;
}

bool allocator_tracking_has_leak(const allocator_tracking_t *tracker)
{
    ASSERT(tracker != NULL);
    return tracker->live_blocks != 0;
}

// =============================================================================
//  Arena 适配器
// =============================================================================
static void *arena_adapter_alloc(allocator_t *self, usize size, usize alignment)
{
    arena_allocator_t *adapter = (arena_allocator_t *)self;
    return arena_alloc_aligned(adapter->arena, size, alignment);
}

static void arena_adapter_free(allocator_t *self, void *ptr, usize size)
{
    // arena 不支持单独释放。这不是"忘了实现"，而是设计如此：
    // 见 memory.h 里关于适用场景的说明。
    ENGINE_UNUSED(self);
    ENGINE_UNUSED(ptr);
    ENGINE_UNUSED(size);
}

void arena_allocator_init(arena_allocator_t *adapter, arena_t *arena)
{
    ASSERT(adapter != NULL);
    ASSERT(arena != NULL);

    adapter->base.alloc = arena_adapter_alloc;
    adapter->base.free = arena_adapter_free;
    adapter->base.ctx = adapter;
    adapter->arena = arena;
}

// =============================================================================
//  Arena
// =============================================================================

// 块结构定义放在 .c 里而不是头文件：外部只需要 arena_t，不需要知道块的内部
// 布局。这样以后想换块管理策略（比如改成固定大小池）不用改头文件。
struct arena_block_t {
    arena_block_t *next;
    usize          capacity; // 数据区容量（不含块头）
    usize          used;     // 数据区已用字节
};

// 块头占用的字节数，向上对齐到 16，保证块内数据起始地址也是 16 字节对齐。
#define ARENA_BLOCK_HEADER_SIZE ENGINE_ALIGN_UP(sizeof(arena_block_t), 16u)

// arena 的最小分配粒度。所有分配都按 16 字节向上取整，这样 blk->used
// 永远是 16 的倍数，16 字节对齐的分配不需要任何额外填充。
#define ARENA_MIN_ALIGNMENT 16u

static u8 *arena_block_data(arena_block_t *block)
{
    return (u8 *)block + ARENA_BLOCK_HEADER_SIZE;
}

static arena_block_t *arena_block_create(arena_t *arena, usize capacity)
{
    ASSERT(arena->alloc != NULL);

    usize total = ARENA_BLOCK_HEADER_SIZE + capacity;

    arena_block_t *block = (arena_block_t *)arena->alloc->alloc(arena->alloc, total, 16);
    if (block == NULL) {
        return NULL;
    }

    block->next     = NULL;
    block->capacity = capacity;
    block->used     = 0;

    arena->block_count++;
    arena->bytes_reserved += capacity;
    return block;
}

bool arena_init(arena_t *arena, allocator_t *alloc, usize block_size, const char *name)
{
    ASSERT(arena != NULL);
    ASSERT(alloc != NULL);

    memset(arena, 0, sizeof(*arena));

    if (block_size == 0) {
        block_size = ENGINE_KB(64);
    }
    block_size = ENGINE_ALIGN_UP(block_size, ARENA_MIN_ALIGNMENT);

    arena->alloc       = alloc;
    arena->block_size  = block_size;
    arena->name        = name;
    arena->owns_blocks = true;

    arena_block_t *block = arena_block_create(arena, block_size);
    if (block == NULL) {
        return false;
    }

    arena->head    = block;
    arena->current = block;
    return true;
}

bool arena_init_from_buffer(arena_t *arena, void *buffer, usize buffer_size, const char *name)
{
    ASSERT(arena != NULL);
    ASSERT(buffer != NULL);
    ASSERT(((usize)(uintptr_t)buffer % 16u) == 0u);
    ASSERT(buffer_size > ARENA_BLOCK_HEADER_SIZE);

    memset(arena, 0, sizeof(*arena));

    // 外部缓冲模式：alloc 为 NULL 意味着块用尽后不会再向系统要内存。
    // 这让 arena 可以完全跑在栈上或静态区里，做到真正的零系统调用。
    arena->alloc       = NULL;
    arena->block_size  = 0;
    arena->name        = name;
    arena->owns_blocks = false;

    arena_block_t *block = (arena_block_t *)buffer;
    block->next          = NULL;
    block->capacity      = buffer_size - ARENA_BLOCK_HEADER_SIZE;
    block->used          = 0;

    arena->head           = block;
    arena->current        = block;
    arena->block_count    = 1;
    arena->bytes_reserved = block->capacity;
    return true;
}

void arena_destroy(arena_t *arena)
{
    if (arena == NULL) {
        return;
    }

    arena_block_t *block = arena->head;
    while (block != NULL) {
        arena_block_t *next = block->next;
        if (arena->owns_blocks && arena->alloc != NULL) {
            arena->alloc->free(arena->alloc, block, ARENA_BLOCK_HEADER_SIZE + block->capacity);
        }
        block = next;
    }

    memset(arena, 0, sizeof(*arena));
}

static void arena_note_allocation(arena_t *arena, usize bytes)
{
    arena->bytes_used += bytes;
    if (arena->bytes_used > arena->peak_bytes_used) {
        arena->peak_bytes_used = arena->bytes_used;
    }
}

// 当前块放不下时走这里：开新块。外部缓冲模式下直接失败。
static void *arena_alloc_new_block(arena_t *arena, usize size, usize alignment)
{
    if (arena->alloc == NULL) {
        return NULL;
    }

    // 新块容量要覆盖"最坏情况的对齐浪费"：块内数据区起点已 16 字节对齐，
    // 但对齐要求可能更大，最多浪费 alignment - 1 字节。
    usize needed   = size + alignment;
    usize capacity = ENGINE_MAX(arena->block_size, ENGINE_ALIGN_UP(needed, ARENA_MIN_ALIGNMENT));

    arena_block_t *block = arena_block_create(arena, capacity);
    if (block == NULL) {
        return NULL;
    }

    if (arena->head == NULL) {
        arena->head = block;
    } else {
        arena->current->next = block;
    }
    arena->current = block;

    u8       *base  = arena_block_data(block);
    uintptr_t addr  = (uintptr_t)base;
    usize     pad   = (usize)(ENGINE_ALIGN_UP(addr, alignment) - addr);

    ASSERT(pad + size <= block->capacity);

    block->used = pad + size;
    arena_note_allocation(arena, block->used);

    return (void *)(addr + pad);
}

void *arena_alloc_aligned(arena_t *arena, usize size, usize alignment)
{
    ASSERT(arena != NULL);

    if (size == 0) {
        return NULL;
    }
    if (alignment < ARENA_MIN_ALIGNMENT) {
        alignment = ARENA_MIN_ALIGNMENT;
    }
    ASSERT(ENGINE_IS_POW2(alignment));

    // 统一向上取整，使 blk->used 始终保持对齐倍数，
    // 后续同对齐级别的分配就不需要任何填充。
    size = ENGINE_ALIGN_UP(size, ARENA_MIN_ALIGNMENT);

    arena_block_t *block = arena->current;
    if (block != NULL) {
        u8 *base = arena_block_data(block);

        // 关键：对齐必须基于**绝对地址**计算，而不是块内偏移。
        // 块头是 16 字节对齐的，所以 base 只保证 16 字节对齐；当请求
        // 32/64/256 字节对齐时，base 本身就不是对齐的，用
        // ENGINE_ALIGN_UP(block->used, alignment) 算出来的偏移量
        // 会得到一个"相对 base 对齐"但绝对地址不对齐的指针。
        // 这种 bug 在只用 16 字节对齐时完全看不出来，等 SIMD 一上就崩。
        const uintptr_t address = (uintptr_t)base + (uintptr_t)block->used;
        const usize     offset  = (usize)(ENGINE_ALIGN_UP(address, alignment) - (uintptr_t)base);

        if (offset + size <= block->capacity) {
            void *ptr = base + offset;
            arena_note_allocation(arena, (offset - block->used) + size);
            block->used = offset + size;
            return ptr;
        }
    }

    return arena_alloc_new_block(arena, size, alignment);
}

void *arena_alloc(arena_t *arena, usize size)
{
    return arena_alloc_aligned(arena, size, ARENA_MIN_ALIGNMENT);
}

void *arena_alloc_zero(arena_t *arena, usize size)
{
    void *ptr = arena_alloc(arena, size);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *arena_alloc_aligned_zero(arena_t *arena, usize size, usize alignment)
{
    void *ptr = arena_alloc_aligned(arena, size, alignment);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void arena_reset(arena_t *arena)
{
    ASSERT(arena != NULL);

    // 保留所有块、只把游标归零：这就是"稳态下零系统调用"的关键。
    // 第一帧可能申请了 8 个块，之后每一帧都复用这 8 个块。
    for (arena_block_t *block = arena->head; block != NULL; block = block->next) {
        block->used = 0;
    }

    arena->current    = arena->head;
    arena->bytes_used = 0;
}

arena_mark_t arena_mark(const arena_t *arena)
{
    ASSERT(arena != NULL);

    arena_mark_t mark;
    mark.block = arena->current;
    mark.used  = (arena->current != NULL) ? arena->current->used : 0;
    return mark;
}

void arena_rewind(arena_t *arena, arena_mark_t mark)
{
    ASSERT(arena != NULL);
    ASSERT(mark.block != NULL);

    // 释放 mark 之后新建的块。这比只回退游标更重要：否则一次"分配 1GB 后失败"
    // 会让那 1GB 永久挂在 arena 上。
    arena_block_t *block = mark.block->next;
    while (block != NULL) {
        arena_block_t *next = block->next;
        arena->bytes_reserved -= block->capacity;
        arena->block_count--;
        if (arena->owns_blocks && arena->alloc != NULL) {
            arena->alloc->free(arena->alloc, block, ARENA_BLOCK_HEADER_SIZE + block->capacity);
        }
        block = next;
    }

    mark.block->next = NULL;
    mark.block->used = mark.used;
    arena->current   = mark.block;

    // bytes_used 是各块 used 之和，重算一次最省心（块数量很少）。
    usize used = 0;
    for (arena_block_t *it = arena->head; it != NULL; it = it->next) {
        used += it->used;
    }
    arena->bytes_used = used;
}

usize arena_bytes_used(const arena_t *arena)
{
    ASSERT(arena != NULL);
    return arena->bytes_used;
}

usize arena_bytes_reserved(const arena_t *arena)
{
    ASSERT(arena != NULL);
    return arena->bytes_reserved;
}

usize arena_block_count(const arena_t *arena)
{
    ASSERT(arena != NULL);
    return arena->block_count;
}

const char *arena_name(const arena_t *arena)
{
    ASSERT(arena != NULL);
    return arena->name;
}

// =============================================================================
//  Pool
// =============================================================================

// 代际取值范围 [1, 255]，跳过 0 以保证任何有效句柄都不等于 HANDLE_INVALID。
#define POOL_GENERATION_MAX 255u

bool pool_init(pool_t *pool, allocator_t *alloc, usize slot_size, usize slot_count,
               const char *name)
{
    ASSERT(pool != NULL);
    ASSERT(alloc != NULL);
    ASSERT(slot_size > 0);
    ASSERT(slot_count > 0);

    memset(pool, 0, sizeof(*pool));

    // 0 号槽永久保留，使 handle_make(0, 0) == HANDLE_INVALID 天然成立，
    // 不需要在每次校验时特判"索引 0 且代际 0"这种组合。
    u32 capacity = (u32)slot_count + 1u;

    pool->slot_size  = (u32)slot_size;
    pool->stride     = (u32)ENGINE_ALIGN_UP(slot_size, 16);
    pool->capacity   = capacity;
    pool->alloc      = alloc;
    pool->name       = name;
    pool->owns_memory = true;

    usize slots_bytes = sizeof(pool_slot_t) * (usize)capacity;
    pool->slots       = (pool_slot_t *)alloc->alloc(alloc, slots_bytes, 16);
    if (pool->slots == NULL) {
        return false;
    }

    usize data_bytes = (usize)pool->stride * (usize)capacity;
    pool->data       = (u8 *)alloc->alloc(alloc, data_bytes, 16);
    if (pool->data == NULL) {
        alloc->free(alloc, pool->slots, slots_bytes);
        pool->slots = NULL;
        return false;
    }

    pool->slots[0].generation = 1;
    pool->slots[0].in_use     = 1; // 永久保留槽，迭代器会跳过
    pool->slots[0].next_free  = POOL_NIL;

    for (u32 i = 1; i < capacity; ++i) {
        pool->slots[i].generation = 1;
        pool->slots[i].in_use     = 0;
        pool->slots[i].next_free  = (i + 1u < capacity) ? (i + 1u) : POOL_NIL;
    }

    pool->free_head  = 1;
    pool->free_count = capacity - 1u;
    return true;
}

void pool_destroy(pool_t *pool)
{
    if (pool == NULL) {
        return;
    }

    if (pool->owns_memory && pool->alloc != NULL) {
        if (pool->slots != NULL) {
            pool->alloc->free(pool->alloc, pool->slots,
                              sizeof(pool_slot_t) * (usize)pool->capacity);
        }
        if (pool->data != NULL) {
            pool->alloc->free(pool->alloc, pool->data, (usize)pool->stride * (usize)pool->capacity);
        }
    }

    memset(pool, 0, sizeof(*pool));
}

handle_t pool_alloc(pool_t *pool)
{
    ASSERT(pool != NULL);

    if (pool->free_head == POOL_NIL) {
        return HANDLE_INVALID; // 池满
    }

    u32          index = pool->free_head;
    pool_slot_t *slot  = &pool->slots[index];

    pool->free_head = slot->next_free;
    slot->in_use    = 1;
    slot->next_free = POOL_NIL;

    pool->free_count--;
    pool->live_count++;
    pool->alloc_calls++;
    if (pool->live_count > pool->peak_live) {
        pool->peak_live = pool->live_count;
    }

    // 清零再交给调用方：否则新对象会看到上一个对象残留的数据，
    // 表现为"随机出现的幽灵字段"。确定性比省这点 memset 重要得多。
    memset(pool->data + (usize)index * pool->stride, 0, pool->slot_size);

    return handle_make(index, slot->generation);
}

bool pool_free(pool_t *pool, handle_t handle)
{
    ASSERT(pool != NULL);

    u32 index = handle_index(handle);
    if (index == 0 || index >= pool->capacity) {
        return false;
    }

    pool_slot_t *slot = &pool->slots[index];

    if (slot->in_use == 0) {
        return false; // 已经是空闲槽：重复释放
    }
    if (slot->generation != handle_generation(handle)) {
        return false; // 代际不匹配：这是一个陈旧句柄
    }

    slot->generation++;
    if (slot->generation == 0 || slot->generation > POOL_GENERATION_MAX) {
        slot->generation = 1; // 回绕时跳过 0
    }

    slot->in_use    = 0;
    slot->next_free = pool->free_head;
    pool->free_head = index;

    pool->free_count++;
    pool->live_count--;
    pool->free_calls++;
    return true;
}

void *pool_get(const pool_t *pool, handle_t handle)
{
    if (pool == NULL) {
        return NULL;
    }

    u32 index = handle_index(handle);
    if (index == 0 || index >= pool->capacity) {
        return NULL;
    }

    const pool_slot_t *slot = &pool->slots[index];
    if (slot->in_use == 0) {
        return NULL; // 空闲槽
    }
    if (slot->generation != handle_generation(handle)) {
        return NULL; // 悬空句柄：槽位已被复用
    }

    return (void *)(pool->data + (usize)index * pool->stride);
}

bool pool_valid(const pool_t *pool, handle_t handle)
{
    return pool_get(pool, handle) != NULL;
}

u32 pool_capacity(const pool_t *pool)
{
    ASSERT(pool != NULL);
    return pool->capacity - 1u; // 减去保留的 0 号槽
}

u32 pool_count(const pool_t *pool)
{
    ASSERT(pool != NULL);
    return pool->live_count;
}

u32 pool_peak(const pool_t *pool)
{
    ASSERT(pool != NULL);
    return pool->peak_live;
}

usize pool_slot_size(const pool_t *pool)
{
    ASSERT(pool != NULL);
    return pool->slot_size;
}

const char *pool_name(const pool_t *pool)
{
    ASSERT(pool != NULL);
    return pool->name;
}

void pool_iter_begin(pool_iter_t *it, const pool_t *pool)
{
    ASSERT(it != NULL);
    ASSERT(pool != NULL);

    it->pool  = pool;
    it->index = 1; // 跳过保留的 0 号槽
}

bool pool_iter_next(pool_iter_t *it, handle_t *out_handle, void **out_ptr)
{
    ASSERT(it != NULL);
    ASSERT(it->pool != NULL);

    const pool_t *pool = it->pool;

    while (it->index < pool->capacity) {
        u32 index = it->index++;
        const pool_slot_t *slot = &pool->slots[index];
        if (slot->in_use == 0) {
            continue; // 空闲槽
        }
        if (out_handle != NULL) {
            *out_handle = handle_make(index, slot->generation);
        }
        if (out_ptr != NULL) {
            *out_ptr = (void *)(pool->data + (usize)index * pool->stride);
        }
        return true;
    }

    return false;
}
