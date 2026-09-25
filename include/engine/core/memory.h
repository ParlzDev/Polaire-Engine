// =============================================================================
//  engine/core/memory.h —— 分配器抽象、Arena、Pool
//
//  三层职责：
//
//    1. allocator_t        分配器接口（函数指针表）。所有需要"会分配内存"的
//                          模块都接收它，而不是直接调 malloc —— 这样测试里可以
//                          换成追踪分配器，生产里可以换成池化分配器。
//
//    2. arena_t            bump 分配器。只前进不回收，整块 reset。适合帧内
//                          临时数据、加载期一次性数据。分配是 O(1) 且只有
//                          几个分支，这就是"帧内零 malloc"的落点。
//
//    3. pool_t             定长槽位分配器，配代际句柄。适合数量稳定、生命周期
//                          不一致的"永久对象"（组件池、资源槽）。
//
//  线程模型（务必看清）：
//    * arena 与 pool 都**不是**线程安全的，且刻意不加锁。它们的正确用法是
//      "一个线程一个 arena"（例如主线程帧 arena、加载线程加载 arena）。
//      跨线程共享请由上层加锁或改用每线程独立实例。
//    * allocator_t 的线程安全性由具体实现决定；allocator_heap 是线程安全的。
// =============================================================================
#ifndef ENGINE_CORE_MEMORY_H
#define ENGINE_CORE_MEMORY_H

#include "engine/core/types.h"

// =============================================================================
//  分配器接口
// =============================================================================

typedef struct allocator_t allocator_t;

struct allocator_t {
    // alignment 必须是 2 的幂。size 为 0 时返回 NULL。
    void *(*alloc)(allocator_t *self, usize size, usize alignment);

    // 为什么 free 要带 size（而不是像标准 free 那样只给指针）：
    //  * 追踪分配器靠它统计存活字节数，否则必须在每次分配前塞一个尺寸头，
    //    而尺寸头会破坏调用方要求的对齐。
    //  * arena / pool 之类的"分配器自己就是内存来源"的实现，free 可以是空操作，
    //    但仍然需要知道这次释放对应多少字节来更新统计。
    //  * 代价是调用方必须记住自己申请了多大 —— 本引擎内部所有调用点都天然知道。
    // ptr 为 NULL 时安全无操作；size 在 ptr 非 NULL 时必须与分配时一致。
    void (*free)(allocator_t *self, void *ptr, usize size);

    void *ctx;
};

// 进程堆分配器（Windows 下走 _aligned_malloc）。线程安全，永不返回 NULL 之外。
allocator_t *allocator_heap(void);

// -----------------------------------------------------------------------------
//  追踪分配器
//
//  包装另一个分配器并统计调用次数与存活字节数。用途：
//    * 单元测试里断言"arena/pool 用完后 live_blocks == 0"，把"无泄漏"从
//      口号变成可执行断言。
//    * 运行期内存面板的数据源。
//
//  注意：base 必须是第一个成员，这样才能安全地 (allocator_t*)&tracker。
// -----------------------------------------------------------------------------
typedef struct allocator_tracking_t {
    allocator_t  base; // 必须位于首位
    allocator_t *inner;

    usize alloc_calls;
    usize free_calls;
    usize live_blocks;   // 已分配未释放的块数
    usize bytes_in_use;  // 当前存活字节数
    usize peak_bytes;    // 峰值存活字节数
    usize total_bytes;   // 累计分配字节数（只增不减）
} allocator_tracking_t;

void allocator_tracking_init(allocator_tracking_t *tracker, allocator_t *inner);
void allocator_tracking_reset(allocator_tracking_t *tracker);
bool allocator_tracking_has_leak(const allocator_tracking_t *tracker);

// =============================================================================
//  Arena —— bump 分配器
//
//  内部是块链表：当前块 bump 满后挂一个新块继续。reset 把所有块清空并复用，
//  因此稳态下不再触发任何系统分配 —— 这是"帧内零 malloc"的实现方式。
// =============================================================================

typedef struct arena_block_t arena_block_t;

typedef struct arena_t {
    arena_block_t *head;     // 块链表头
    arena_block_t *current;  // 当前 bump 块
    allocator_t   *alloc;    // 新块来源；NULL 表示外部缓冲模式（不再扩块）
    usize          block_size;
    bool           owns_blocks;
    const char    *name;

    usize block_count;
    usize bytes_used;     // 各块已用字节之和
    usize bytes_reserved; // 各块容量之和
    usize peak_bytes_used;
} arena_t;

// 创建 arena。block_size 为单块初始容量，内部会按需扩块。
// name 仅用于调试输出，可为 NULL。
bool arena_init(arena_t *arena, allocator_t *alloc, usize block_size, const char *name);

// 在调用方提供的缓冲上建 arena。缓冲耗尽后不会再向系统要内存，
// 分配失败返回 NULL。用于"帧 arena 放在栈上"或"完全静态、零系统调用"的场景。
// 注意：缓冲需 16 字节对齐，函数内部会占用开头几十字节做块头。
bool arena_init_from_buffer(arena_t *arena, void *buffer, usize buffer_size, const char *name);

void arena_destroy(arena_t *arena);

void *arena_alloc(arena_t *arena, usize size);
void *arena_alloc_aligned(arena_t *arena, usize size, usize alignment);
void *arena_alloc_zero(arena_t *arena, usize size);
void *arena_alloc_aligned_zero(arena_t *arena, usize size, usize alignment);

// 清空所有块但保留已申请的内存，供下一轮复用。不释放任何块。
void arena_reset(arena_t *arena);

// -----------------------------------------------------------------------------
//  标记 / 回退
//
//  比 reset 更细粒度：只回退到某个时间点，保留之前的数据。
//  典型用法是"解析一个文件，中途失败就整体回退"。
// -----------------------------------------------------------------------------
typedef struct arena_mark_t {
    arena_block_t *block;
    usize          used;
} arena_mark_t;

arena_mark_t arena_mark(const arena_t *arena);
void         arena_rewind(arena_t *arena, arena_mark_t mark);

// 统计
usize       arena_bytes_used(const arena_t *arena);
usize       arena_bytes_reserved(const arena_t *arena);
usize       arena_block_count(const arena_t *arena);
const char *arena_name(const arena_t *arena);

// 语法糖：在 arena 上构造单个对象 / 数组，失败返回 NULL。
#define ARENA_NEW(arena, T) ((T *)arena_alloc_zero((arena), sizeof(T)))
#define ARENA_NEW_ARRAY(arena, T, count)                                                           \
    ((T *)arena_alloc_zero((arena), sizeof(T) * (usize)(count)))

// -----------------------------------------------------------------------------
//  Arena 适配器
//
//  把 arena 包装成一个 allocator_t，让"只接受 allocator_t*"的模块
//  （哈希表、动态数组、pool）能跑在 arena 上。
//
//  free 是**空操作** —— arena 不支持单独释放，内存要等 arena_reset 才回收。
//  这意味着用它当后端的容器在扩容时会"泄漏"掉旧的存储块。
//  因此它只适合"生命周期短、扩容次数有限"的场景：
//    * 解析一个文件时用到的哈希表（解析完 arena 就 reset 了）
//    * 加载期一次性构造的容器
//  不要拿它给长生命周期、会频繁增删的容器当后端。
// -----------------------------------------------------------------------------
typedef struct arena_allocator_t {
    allocator_t base; // 必须是第一个成员
    arena_t    *arena;
} arena_allocator_t;

void arena_allocator_init(arena_allocator_t *adapter, arena_t *arena);

// =============================================================================
//  Pool —— 定长槽位 + 代际句柄
//
//  为什么不用 free list 存裸指针：裸指针一旦被复用就会静默指向新对象，
//  表现为"已删除的敌人还在攻击我"这类幽灵 bug。句柄里的代际号让这种情况
//  变成一次 pool_get 返回 NULL。
//
//  容量固定：不做扩容，因为扩容会移动 data 指针，使所有已发放的裸指针失效。
//  需要更多槽位就在初始化时给够。
// =============================================================================

#define POOL_NIL ((u32)0xFFFFFFFFu) // 空闲链表结束标记（也用于 free_head 表示池已满）

typedef struct pool_slot_t {
    // 代际只保留 8 位，与 handle_t 中的 generation 字段宽度一致。
    // 存成 u32 会在超过 255 次复用后与句柄里的截断值对不上，
    // 导致 pool_get 误判所有句柄都失效 —— 这是个很隐蔽的坑。
    u8 generation;

    // 显式的占用标志。不用 next_free == POOL_NIL 来表示"占用"，
    // 因为 POOL_NIL 同时也是空闲链表的结束标记，一个值承担两种含义时，
    // 链表尾部的那个空闲槽会被误判成"正在使用"（迭代器会多遍历一个元素）。
    // 加上这个字段不增加结构体大小（本来就有的对齐填充正好用上）。
    u8 in_use;

    u32 next_free; // 仅在 in_use == 0 时有效；POOL_NIL 表示空闲链表到此结束
} pool_slot_t;

typedef struct pool_t {
    pool_slot_t *slots;
    u8          *data;     // capacity * stride 的裸字节区
    u32          capacity; // 含永久保留的 0 号槽
    u32          stride;   // 每个槽位的字节跨度（已按 16 对齐）
    u32          slot_size;
    u32          live_count;
    u32          peak_live;
    u32          free_head; // 空闲队列头；POOL_NIL 表示池已满
    // 空闲**队列**（不是栈）。释放的槽位排到尾部，分配从头部取。
    //
    // 为什么不是 LIFO：代际只有 8 位（要和 handle_t 的位域对齐），
    // 同一槽位复用 256 次后代际回绕，此时持有旧句柄的代码会**重新看到
    // 一个"有效"的句柄**，指向一个完全不同的对象 —— 表现是"早就销毁的
    // 敌人又活了过来"，且不复现。LIFO 下一个热点槽位几秒就能烧完 256 次；
    // 改成 FIFO 后回收要先把其它空槽发完，回绕周期从 256 放大到
    // 约 256 × 空闲槽数，实际不可达。
    u32          free_tail;
    u32          free_count;
    u32          alloc_calls;
    u32          free_calls;
    allocator_t *alloc;
    bool         owns_memory;
    const char  *name;
} pool_t;

// slot_size 为单个元素的字节数；slot_count 为期望元素数（0 号槽额外保留）。
bool pool_init(pool_t *pool, allocator_t *alloc, usize slot_size, usize slot_count,
               const char *name);
void pool_destroy(pool_t *pool);

// 池满返回 HANDLE_INVALID。
handle_t pool_alloc(pool_t *pool);

// 释放。句柄无效或已释放返回 false（不会崩，便于定位重复释放）。
bool pool_free(pool_t *pool, handle_t handle);

// 取元素指针。句柄无效 / 已释放 / 代际不匹配 都返回 NULL。
void *pool_get(const pool_t *pool, handle_t handle);

// 只做有效性检查，不返回指针。
bool pool_valid(const pool_t *pool, handle_t handle);

u32   pool_capacity(const pool_t *pool);
u32   pool_count(const pool_t *pool);
u32   pool_peak(const pool_t *pool);
usize pool_slot_size(const pool_t *pool);
const char *pool_name(const pool_t *pool);

// -----------------------------------------------------------------------------
//  迭代器
//
//  不用 pool_foreach 宏，是因为嵌套循环 + 提前 break 的宏写法极易出错，
//  而显式迭代器在调试器里可单步、可打断点。性能上两者无差异。
// -----------------------------------------------------------------------------
typedef struct pool_iter_t {
    const pool_t *pool;
    u32           index;
} pool_iter_t;

void pool_iter_begin(pool_iter_t *it, const pool_t *pool);

// 返回 false 表示迭代结束。out_handle / out_ptr 允许为 NULL。
bool pool_iter_next(pool_iter_t *it, handle_t *out_handle, void **out_ptr);

// 语法糖
#define POOL_GET_AS(pool, handle, T) ((T *)pool_get((pool), (handle)))

#endif // ENGINE_CORE_MEMORY_H
