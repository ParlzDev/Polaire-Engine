// =============================================================================
//  engine/core/hash.h —— 开放寻址哈希表（u64 -> u64）
//
//  为什么先只做 u64 -> u64：
//  引擎里 90% 的哈希需求是"句柄 -> 槽位"、"字符串哈希 -> ID"、"ID -> 索引"
//  这类整数映射。把这一种做扎实、做快、做对，比一开始就上泛型宏生成器更有价值。
//  需要结构体值时，把值编码成句柄或索引即可。
//
//  实现要点：
//   * 线性探测 + 2 的幂容量。缓存友好，无需指针追逐。
//   * 删除用"后移删除"（backward shift）而非墓碑标记。墓碑会随删除累积，
//     使探测链越来越长、迭代逻辑变复杂；后移删除让表始终保持紧凑。
//   * 用独立的 states 数组区分"空"与"键为 0"，避免占用某个键值做哨兵。
//   * 负载因子 0.75 触发扩容。
// =============================================================================
#ifndef ENGINE_CORE_HASH_H
#define ENGINE_CORE_HASH_H

#include "engine/core/memory.h"
#include "engine/core/types.h"

typedef struct hash_t {
    u64         *keys;
    u64         *values;
    u8          *states; // 0 = 空，1 = 占用
    u32          capacity; // 2 的幂
    u32          size;
    u32          growth_count;
    allocator_t *alloc;
} hash_t;

bool hash_init(hash_t *map, allocator_t *alloc, u32 initial_capacity);
void hash_destroy(hash_t *map);

// 插入或覆盖。返回 true 表示插入成功（false 只会发生在扩容失败时）。
bool hash_set(hash_t *map, u64 key, u64 value);

// 查询。命中时写 *out_value（允许为 NULL）并返回 true。
bool hash_get(const hash_t *map, u64 key, u64 *out_value);

// 仅判断存在。
bool hash_contains(const hash_t *map, u64 key);

// 删除。键不存在返回 false。
bool hash_remove(hash_t *map, u64 key);

// 清空内容但保留已分配内存。
void hash_clear(hash_t *map);

u32  hash_size(const hash_t *map);
u32  hash_capacity(const hash_t *map);
u32  hash_growth_count(const hash_t *map);

// ---------------------------------------------------------------- 哈希函数
// splitmix64 的收尾混合。输入输出都是 u64，雪崩性足够好。
u64 hash_u64(u64 key);

// FNV-1a 64 位。用于任意字节串。
u64 hash_bytes(const void *data, usize length);

// 以 NUL 结尾的字符串哈希（不含结尾 NUL）。s 必须非空。
u64 hash_str(const char *s);

// ---------------------------------------------------------------- 迭代器
typedef struct hash_iter_t {
    const hash_t *map;
    u32           index;
} hash_iter_t;

void hash_iter_begin(hash_iter_t *it, const hash_t *map);

// 返回 false 表示迭代结束。out_key / out_value 允许为 NULL。
bool hash_iter_next(hash_iter_t *it, u64 *out_key, u64 *out_value);

#endif // ENGINE_CORE_HASH_H
