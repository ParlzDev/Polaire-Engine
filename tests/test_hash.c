// =============================================================================
//  tests/test_hash.c —— 开放寻址哈希表
//
//  删除是本表最危险的操作：删错一个元素会让整条探测链断裂，
//  表现为"某些键莫名其妙查不到"。所以这里既做了针对性的定向测试，
//  也用了一个朴素参考实现做随机对拍。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

static void init_and_destroy_without_leak(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    hash_t map;
    EXPECT_TRUE(hash_init(&map, &tracker.base, 0));
    EXPECT_EQ_U64(hash_size(&map), 0u);
    EXPECT_GE_U64(hash_capacity(&map), 16u);
    EXPECT_TRUE(ENGINE_IS_POW2(hash_capacity(&map)));

    hash_destroy(&map);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void set_get_contains_remove(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    hash_t map;
    EXPECT_TRUE(hash_init(&map, &tracker.base, 0));

    EXPECT_TRUE(hash_set(&map, 1, 100));
    EXPECT_TRUE(hash_set(&map, 2, 200));
    EXPECT_EQ_U64(hash_size(&map), 2u);

    u64 value = 0;
    EXPECT_TRUE(hash_get(&map, 1, &value));
    EXPECT_EQ_U64(value, 100u);
    EXPECT_TRUE(hash_get(&map, 2, &value));
    EXPECT_EQ_U64(value, 200u);

    EXPECT_TRUE(hash_contains(&map, 1));
    EXPECT_FALSE(hash_contains(&map, 3));
    EXPECT_FALSE(hash_get(&map, 3, &value));

    // 覆盖：size 不变，值更新
    EXPECT_TRUE(hash_set(&map, 1, 999));
    EXPECT_EQ_U64(hash_size(&map), 2u);
    EXPECT_TRUE(hash_get(&map, 1, &value));
    EXPECT_EQ_U64(value, 999u);

    EXPECT_TRUE(hash_remove(&map, 1));
    EXPECT_EQ_U64(hash_size(&map), 1u);
    EXPECT_FALSE(hash_get(&map, 1, &value));
    EXPECT_FALSE(hash_contains(&map, 1));
    EXPECT_FALSE(hash_remove(&map, 1)); // 重复删除返回 false

    // 删掉的键可以重新插入
    EXPECT_TRUE(hash_set(&map, 1, 555));
    EXPECT_TRUE(hash_get(&map, 1, &value));
    EXPECT_EQ_U64(value, 555u);

    // out_value 允许为 NULL
    EXPECT_TRUE(hash_get(&map, 1, NULL));

    hash_destroy(&map);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void zero_key_is_not_special(void)
{
    // 用独立的 states 数组区分"空槽"与"键为 0"，
    // 因此 0 是一个完全普通的键。
    hash_t map;
    EXPECT_TRUE(hash_init(&map, allocator_heap(), 0));

    EXPECT_TRUE(hash_set(&map, 0, 12345));
    EXPECT_EQ_U64(hash_size(&map), 1u);

    u64 value = 0;
    EXPECT_TRUE(hash_get(&map, 0, &value));
    EXPECT_EQ_U64(value, 12345u);

    EXPECT_TRUE(hash_remove(&map, 0));
    EXPECT_FALSE(hash_contains(&map, 0));
    EXPECT_EQ_U64(hash_size(&map), 0u);

    hash_destroy(&map);
}

static void grows_and_keeps_all_keys(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    hash_t map;
    EXPECT_TRUE(hash_init(&map, &tracker.base, 0));

    const u32 count = 5000;
    for (u32 i = 0; i < count; ++i) {
        if (!hash_set(&map, (u64)i * 7u + 1u, (u64)i)) {
            EXPECT_TRUE(false);
            break;
        }
    }

    EXPECT_EQ_U64(hash_size(&map), count);
    EXPECT_GT_U64(hash_growth_count(&map), 0u);
    // 负载因子 0.75 的上界
    EXPECT_TRUE((u64)hash_size(&map) * 4u <= (u64)hash_capacity(&map) * 3u);

    for (u32 i = 0; i < count; ++i) {
        u64 value = 0;
        if (!hash_get(&map, (u64)i * 7u + 1u, &value)) {
            EXPECT_TRUE(false);
            break;
        }
        if (value != (u64)i) {
            EXPECT_EQ_U64(value, (u64)i);
            break;
        }
    }

    hash_destroy(&map);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void remove_keeps_probe_chains_intact(void)
{
    // 定向验证后移删除：删掉一半元素后，剩下的必须全都还能查到。
    // 如果删除只是把槽位标空而没搬移后续元素，这个用例会挂。
    hash_t map;
    EXPECT_TRUE(hash_init(&map, allocator_heap(), 64));

    const u32 count = 2000;
    for (u32 i = 0; i < count; ++i) {
        hash_set(&map, i, (u64)i * 2u);
    }

    for (u32 i = 0; i < count; i += 2) {
        EXPECT_TRUE(hash_remove(&map, i));
    }

    EXPECT_EQ_U64(hash_size(&map), count / 2u);

    for (u32 i = 0; i < count; ++i) {
        u64 value = 0;
        if ((i % 2u) == 0u) {
            EXPECT_FALSE(hash_get(&map, i, &value));
        } else {
            EXPECT_TRUE(hash_get(&map, i, &value));
            EXPECT_EQ_U64(value, (u64)i * 2u);
        }
    }

    hash_destroy(&map);
}

static void remove_adjacent_cluster(void)
{
    // 连续删除一整段（最容易触发后移删除的边界情况）
    hash_t map;
    EXPECT_TRUE(hash_init(&map, allocator_heap(), 0));

    for (u64 i = 0; i < 100; ++i) {
        hash_set(&map, i, i + 1000u);
    }

    // 删掉中间连续 50 个
    for (u64 i = 25; i < 75; ++i) {
        EXPECT_TRUE(hash_remove(&map, i));
    }

    for (u64 i = 0; i < 100; ++i) {
        u64 value = 0;
        if (i >= 25 && i < 75) {
            EXPECT_FALSE(hash_get(&map, i, &value));
        } else {
            EXPECT_TRUE(hash_get(&map, i, &value));
            EXPECT_EQ_U64(value, i + 1000u);
        }
    }

    hash_destroy(&map);
}

static void randomized_matches_naive_reference(void)
{
    // 用朴素的"数组线性查找"作为参考实现，随机做插入/删除/查询并逐步对拍。
    // 这是覆盖"删除后探测链"这类组合边界最有效的手段 ——
    // 手写用例很难穷举出所有删除顺序。
    enum { REF_CAPACITY = 256 };

    u64  ref_keys[REF_CAPACITY];
    u64  ref_values[REF_CAPACITY];
    bool ref_used[REF_CAPACITY];
    memset(ref_used, 0, sizeof(ref_used));

    hash_t map;
    EXPECT_TRUE(hash_init(&map, allocator_heap(), 16));

    u64 rng = 0x2545F4914F6CDD1DULL;
    u32 live = 0;

    for (u32 step = 0; step < 5000; ++step) {
        // xorshift64
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;

        const u32 operation = (u32)(rng % 3u);
        const u64 key = (rng >> 11) % 200u; // 键空间故意小于操作数，制造大量覆盖与删除

        if (operation == 0) {
            // 插入 / 覆盖
            const u64 value = rng ^ 0xDEADBEEFULL;
            hash_set(&map, key, value);

            bool found = false;
            for (u32 i = 0; i < REF_CAPACITY; ++i) {
                if (ref_used[i] && ref_keys[i] == key) {
                    ref_values[i] = value;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (u32 i = 0; i < REF_CAPACITY; ++i) {
                    if (!ref_used[i]) {
                        ref_used[i] = true;
                        ref_keys[i] = key;
                        ref_values[i] = value;
                        live++;
                        break;
                    }
                }
            }
        } else if (operation == 1) {
            const bool removed = hash_remove(&map, key);

            bool ref_removed = false;
            for (u32 i = 0; i < REF_CAPACITY; ++i) {
                if (ref_used[i] && ref_keys[i] == key) {
                    ref_used[i] = false;
                    ref_removed = true;
                    live--;
                    break;
                }
            }
            EXPECT_EQ_U64(removed, ref_removed);
        } else {
            u64        got = 0;
            const bool found = hash_get(&map, key, &got);

            bool ref_found = false;
            u64  ref_value = 0;
            for (u32 i = 0; i < REF_CAPACITY; ++i) {
                if (ref_used[i] && ref_keys[i] == key) {
                    ref_found = true;
                    ref_value = ref_values[i];
                    break;
                }
            }

            EXPECT_EQ_U64(found, ref_found);
            if (found && ref_found) {
                EXPECT_EQ_U64(got, ref_value);
            }
        }

        if (hash_size(&map) != live) {
            EXPECT_EQ_U64(hash_size(&map), live);
            break;
        }
    }

    hash_destroy(&map);
}

static void iteration_visits_each_entry_once(void)
{
    hash_t map;
    EXPECT_TRUE(hash_init(&map, allocator_heap(), 0));

    const u32 count = 500;
    for (u32 i = 0; i < count; ++i) {
        hash_set(&map, i, i + 1u);
    }
    for (u32 i = 0; i < count; i += 3) {
        hash_remove(&map, i);
    }

    u64 key_sum = 0;
    u64 value_sum = 0;
    u32 visited = 0;

    hash_iter_t it;
    hash_iter_begin(&it, &map);

    u64 key = 0;
    u64 value = 0;
    while (hash_iter_next(&it, &key, &value)) {
        visited++;
        key_sum += key;
        value_sum += value;
    }

    EXPECT_EQ_U64(visited, hash_size(&map));

    u64 expected_key_sum = 0;
    u64 expected_value_sum = 0;
    for (u32 i = 0; i < count; ++i) {
        if ((i % 3u) != 0u) {
            expected_key_sum += i;
            expected_value_sum += i + 1u;
        }
    }
    EXPECT_EQ_U64(key_sum, expected_key_sum);
    EXPECT_EQ_U64(value_sum, expected_value_sum);

    hash_destroy(&map);
}

static void clear_keeps_capacity(void)
{
    hash_t map;
    EXPECT_TRUE(hash_init(&map, allocator_heap(), 0));

    for (u32 i = 0; i < 100; ++i) {
        hash_set(&map, i, i);
    }
    const u32 capacity = hash_capacity(&map);

    hash_clear(&map);
    EXPECT_EQ_U64(hash_size(&map), 0u);
    EXPECT_EQ_U64(hash_capacity(&map), capacity);
    EXPECT_FALSE(hash_contains(&map, 50));

    // 清空后可以继续使用
    EXPECT_TRUE(hash_set(&map, 7, 77));
    u64 value = 0;
    EXPECT_TRUE(hash_get(&map, 7, &value));
    EXPECT_EQ_U64(value, 77u);

    hash_destroy(&map);
}

static void hash_functions_are_deterministic(void)
{
    // 哈希值一旦变化，序列化格式、网络协议、存档兼容性都会受影响。
    // 这里把 FNV-1a 的空串结果钉死，防止有人"顺手优化"了哈希函数。
    EXPECT_EQ_U64(hash_bytes("", 0), 0xCBF29CE484222325ULL);
    EXPECT_EQ_U64(hash_str("a"), hash_bytes("a", 1));
    EXPECT_EQ_U64(hash_str("hello"), hash_str("hello"));
    EXPECT_TRUE(hash_str("hello") != hash_str("hellp"));
    EXPECT_TRUE(hash_str("") != hash_str("a"));

    // 长度必须参与哈希，否则 "ab" 与 "a" 加一个 0 字节会撞
    const char bytes_a[] = {'a', 'b'};
    const char bytes_b[] = {'a', '\0'};
    EXPECT_TRUE(hash_bytes(bytes_a, 2) != hash_bytes(bytes_b, 2));
}

static void hash_u64_has_good_avalanche(void)
{
    // 粗测低位分布。线性探测用低位决定初始槽位，
    // 低位分布差会直接变成严重聚集（性能塌陷），所以值得单独测。
    enum { BUCKETS = 64, SAMPLES = 6400 };
    u32 histogram[BUCKETS] = {0};

    for (u32 i = 0; i < SAMPLES; ++i) {
        histogram[hash_u64(i) & (BUCKETS - 1)]++;
    }

    const u32 expected = SAMPLES / BUCKETS;
    u32 min_count = 0xFFFFFFFFu;
    u32 max_count = 0;
    for (u32 i = 0; i < BUCKETS; ++i) {
        if (histogram[i] < min_count) {
            min_count = histogram[i];
        }
        if (histogram[i] > max_count) {
            max_count = histogram[i];
        }
    }

    // 完美均匀是 100，给一个宽松但仍有意义的界
    EXPECT_GE_U64(min_count, expected / 2u);
    EXPECT_LE_U64(max_count, expected * 2u);

    // 相邻输入不能映射到同一个值
    EXPECT_TRUE(hash_u64(0) != hash_u64(1));
    EXPECT_TRUE(hash_u64(1) != hash_u64(2));
    // 输入只改 1 位，输出应显著不同（雪崩）
    u32 differing_bits = 0;
    for (u32 bit = 0; bit < 32; ++bit) {
        const u64 a = hash_u64(0);
        const u64 b = hash_u64(1ULL << bit);
        u64       diff = a ^ b;
        while (diff != 0) {
            differing_bits += (u32)(diff & 1u);
            diff >>= 1;
        }
    }
    // 32 次试验，每次期望约 32 位翻转，总计约 1024
    EXPECT_GT_U64(differing_bits, 800u);
}

void test_hash_register(void)
{
    test_register("hash", "init_and_destroy_without_leak", init_and_destroy_without_leak);
    test_register("hash", "set_get_contains_remove", set_get_contains_remove);
    test_register("hash", "zero_key_is_not_special", zero_key_is_not_special);
    test_register("hash", "grows_and_keeps_all_keys", grows_and_keeps_all_keys);
    test_register("hash", "remove_keeps_probe_chains_intact", remove_keeps_probe_chains_intact);
    test_register("hash", "remove_adjacent_cluster", remove_adjacent_cluster);
    test_register("hash", "randomized_matches_naive_reference", randomized_matches_naive_reference);
    test_register("hash", "iteration_visits_each_entry_once", iteration_visits_each_entry_once);
    test_register("hash", "clear_keeps_capacity", clear_keeps_capacity);
    test_register("hash", "hash_functions_are_deterministic", hash_functions_are_deterministic);
    test_register("hash", "hash_u64_has_good_avalanche", hash_u64_has_good_avalanche);
}
