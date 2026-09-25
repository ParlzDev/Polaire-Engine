// =============================================================================
//  src/core/hash.c —— 开放寻址哈希表实现
//
//  探测策略：线性探测 + 2 的幂容量。
//  删除策略：后移删除（backward shift），不用墓碑。
//    墓碑方案的问题是删除会在表里留下"坑"，随删除次数增加探测链越来越长，
//    而且迭代时必须跳过墓碑、查询时必须把墓碑当作"继续找"的信号，
//    三处逻辑都要小心。后移删除让表在删除后依然保持"无空洞"，
//    查询、插入、迭代三处逻辑都能保持最简单的形式。
//    代价是删除时可能需要搬动后续元素，但摊还成本是常数级。
// =============================================================================
#include "engine/core/hash.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/math.h"

#define HASH_STATE_EMPTY ((u8)0)
#define HASH_STATE_USED ((u8)1)

#define HASH_NOT_FOUND ((u32)0xFFFFFFFFu)

#define HASH_MIN_CAPACITY 16u

// 负载因子 3/4。取 3/4 而不是 7/8：线性探测在负载率超过 3/4 后，
// 平均探测长度会急剧上升（聚集效应），查询延迟变得不可预测。
#define HASH_LOAD_NUMERATOR 3u
#define HASH_LOAD_DENOMINATOR 4u

static u32 hash_round_capacity(u32 requested)
{
    if (requested < HASH_MIN_CAPACITY) {
        return HASH_MIN_CAPACITY;
    }
    u32 rounded = math_next_pow2_u32(requested);
    return (rounded == 0) ? HASH_MIN_CAPACITY : rounded;
}

// 重新分配并重新插入所有元素。
static bool hash_rehash(hash_t *map, u32 new_capacity)
{
    u64 *new_keys   = (u64 *)map->alloc->alloc(map->alloc, sizeof(u64) * new_capacity, 16);
    if (new_keys == NULL) {
        return false;
    }

    u64 *new_values = (u64 *)map->alloc->alloc(map->alloc, sizeof(u64) * new_capacity, 16);
    if (new_values == NULL) {
        map->alloc->free(map->alloc, new_keys, sizeof(u64) * new_capacity);
        return false;
    }

    u8 *new_states = (u8 *)map->alloc->alloc(map->alloc, sizeof(u8) * new_capacity, 16);
    if (new_states == NULL) {
        map->alloc->free(map->alloc, new_keys, sizeof(u64) * new_capacity);
        map->alloc->free(map->alloc, new_values, sizeof(u64) * new_capacity);
        return false;
    }

    memset(new_states, HASH_STATE_EMPTY, new_capacity);

    const u32 new_mask = new_capacity - 1u;

    for (u32 i = 0; i < map->capacity; ++i) {
        if (map->states == NULL || map->states[i] != HASH_STATE_USED) {
            continue;
        }
        u64 key = map->keys[i];
        u32 idx = (u32)(hash_u64(key) & new_mask);
        while (new_states[idx] == HASH_STATE_USED) {
            idx = (idx + 1u) & new_mask;
        }
        new_keys[idx]   = key;
        new_values[idx] = map->values[i];
        new_states[idx] = HASH_STATE_USED;
    }

    if (map->keys != NULL) {
        map->alloc->free(map->alloc, map->keys, sizeof(u64) * map->capacity);
    }
    if (map->values != NULL) {
        map->alloc->free(map->alloc, map->values, sizeof(u64) * map->capacity);
    }
    if (map->states != NULL) {
        map->alloc->free(map->alloc, map->states, sizeof(u8) * map->capacity);
    }

    map->keys     = new_keys;
    map->values   = new_values;
    map->states   = new_states;
    map->capacity = new_capacity;
    return true;
}

// 返回键所在槽位下标；未找到返回 HASH_NOT_FOUND。
static u32 hash_find_slot(const hash_t *map, u64 key)
{
    const u32 mask = map->capacity - 1u;
    u32       idx  = (u32)(hash_u64(key) & mask);

    while (map->states[idx] == HASH_STATE_USED) {
        if (map->keys[idx] == key) {
            return idx;
        }
        idx = (idx + 1u) & mask;
    }
    return HASH_NOT_FOUND;
}

bool hash_init(hash_t *map, allocator_t *alloc, u32 initial_capacity)
{
    ASSERT(map != NULL);
    ASSERT(alloc != NULL);

    memset(map, 0, sizeof(*map));
    map->alloc = alloc;

    return hash_rehash(map, hash_round_capacity(initial_capacity));
}

void hash_destroy(hash_t *map)
{
    if (map == NULL) {
        return;
    }

    if (map->alloc != NULL) {
        if (map->keys != NULL) {
            map->alloc->free(map->alloc, map->keys, sizeof(u64) * map->capacity);
        }
        if (map->values != NULL) {
            map->alloc->free(map->alloc, map->values, sizeof(u64) * map->capacity);
        }
        if (map->states != NULL) {
            map->alloc->free(map->alloc, map->states, sizeof(u8) * map->capacity);
        }
    }

    memset(map, 0, sizeof(*map));
}

bool hash_set(hash_t *map, u64 key, u64 value)
{
    ASSERT(map != NULL);

    if (map->states == NULL) {
        if (!hash_rehash(map, HASH_MIN_CAPACITY)) {
            return false;
        }
    }

    // 先判断"插入这个元素之后"是否超载，而不是等超载了再扩。
    // 否则会有一瞬间表处于 100% 满的状态，那一瞬间的插入会退化成线性扫描。
    const u64 projected = (u64)map->size + 1u;
    if (projected * HASH_LOAD_DENOMINATOR > (u64)map->capacity * HASH_LOAD_NUMERATOR) {
        if (!hash_rehash(map, map->capacity * 2u)) {
            return false;
        }
        map->growth_count++;
    }

    const u32 mask = map->capacity - 1u;
    u32       idx  = (u32)(hash_u64(key) & mask);

    while (map->states[idx] == HASH_STATE_USED) {
        if (map->keys[idx] == key) {
            map->values[idx] = value; // 覆盖，size 不变
            return true;
        }
        idx = (idx + 1u) & mask;
    }

    map->keys[idx]   = key;
    map->values[idx] = value;
    map->states[idx] = HASH_STATE_USED;
    map->size++;
    return true;
}

bool hash_get(const hash_t *map, u64 key, u64 *out_value)
{
    if (map == NULL || map->states == NULL || map->size == 0) {
        return false;
    }

    u32 idx = hash_find_slot(map, key);
    if (idx == HASH_NOT_FOUND) {
        return false;
    }

    if (out_value != NULL) {
        *out_value = map->values[idx];
    }
    return true;
}

bool hash_contains(const hash_t *map, u64 key)
{
    return hash_get(map, key, NULL);
}

bool hash_remove(hash_t *map, u64 key)
{
    if (map == NULL || map->states == NULL || map->size == 0) {
        return false;
    }

    const u32 mask = map->capacity - 1u;
    u32       hole = hash_find_slot(map, key);
    if (hole == HASH_NOT_FOUND) {
        return false;
    }

    map->states[hole] = HASH_STATE_EMPTY;
    map->size--;

    // 后移删除：把"本该落在空洞之后、但因为空洞存在而被挤到后面"的元素依次前移。
    //
    // 两个游标必须独立前进，这是本算法最容易写错的地方：
    //   j 是扫描游标，每轮无条件前进；
    //   i 是当前空洞位置，只在真的搬移了元素时才跟着 j 走。
    // 如果图省事写成 j = (i + 1) & mask（从 i 重新推导），那么当某个元素
    // 不该被搬移时 j 就不会前进，循环卡在原地 —— 表现是删除操作直接挂死。
    //
    // 判据：元素在 j，理想槽位是 home。若从 home 出发到 i 的距离小于到 j 的
    // 距离，说明 i 位于它的探测路径上，搬过去不会破坏任何查询。
    u32 i = hole;
    u32 j = hole;
    for (;;) {
        j = (j + 1u) & mask;
        if (map->states[j] != HASH_STATE_USED) {
            break;
        }

        const u32 home = (u32)(hash_u64(map->keys[j]) & mask);
        if (((i - home) & mask) < ((j - home) & mask)) {
            map->keys[i]   = map->keys[j];
            map->values[i] = map->values[j];
            map->states[i] = HASH_STATE_USED;
            map->states[j] = HASH_STATE_EMPTY;
            i              = j;
        }
    }

    return true;
}

void hash_clear(hash_t *map)
{
    if (map == NULL || map->states == NULL) {
        return;
    }
    memset(map->states, HASH_STATE_EMPTY, map->capacity);
    map->size = 0;
}

u32 hash_size(const hash_t *map)
{
    ASSERT(map != NULL);
    return map->size;
}

u32 hash_capacity(const hash_t *map)
{
    ASSERT(map != NULL);
    return map->capacity;
}

u32 hash_growth_count(const hash_t *map)
{
    ASSERT(map != NULL);
    return map->growth_count;
}

// =============================================================================
//  哈希函数
// =============================================================================

// splitmix64 的收尾混合函数。雪崩性经过验证：输入改 1 位，输出约一半位翻转。
// 为什么不用"乘一个大质数"这种简单方案：那样低位分布很差，
// 而线性探测正好是用低位决定初始槽位的（& mask），低位差会直接变成严重聚集。
u64 hash_u64(u64 key)
{
    key ^= key >> 30;
    key *= 0xBF58476D1CE4E5B9ULL;
    key ^= key >> 27;
    key *= 0x94D049BB133111EBULL;
    key ^= key >> 31;
    return key;
}

u64 hash_bytes(const void *data, usize length)
{
    const u8 *bytes = (const u8 *)data;

    // FNV-1a 64 位。选它是因为实现极短、无查表、对短字符串表现够好。
    u64 hash = 0xCBF29CE484222325ULL; // FNV offset basis
    for (usize i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ULL; // FNV prime
    }
    return hash;
}

u64 hash_str(const char *s)
{
    ASSERT(s != NULL);
    return hash_bytes(s, strlen(s));
}

// =============================================================================
//  迭代器
// =============================================================================
void hash_iter_begin(hash_iter_t *it, const hash_t *map)
{
    ASSERT(it != NULL);
    ASSERT(map != NULL);

    it->map   = map;
    it->index = 0;
}

bool hash_iter_next(hash_iter_t *it, u64 *out_key, u64 *out_value)
{
    ASSERT(it != NULL);
    ASSERT(it->map != NULL);

    const hash_t *map = it->map;
    if (map->states == NULL) {
        return false;
    }

    while (it->index < map->capacity) {
        u32 index = it->index++;
        if (map->states[index] != HASH_STATE_USED) {
            continue;
        }
        if (out_key != NULL) {
            *out_key = map->keys[index];
        }
        if (out_value != NULL) {
            *out_value = map->values[index];
        }
        return true;
    }

    return false;
}
