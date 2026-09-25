// =============================================================================
//  engine/ecs/query.h —— 查询（view<A,B> 的 C 写法）
//
//  ---- 语义 ----
//
//  all  : 实体必须**同时**拥有这些组件
//  none : 实体不能拥有这些组件（排除集，系统之间分工就靠它）
//
//  ---- 驱动类型怎么选（决策 D1 的关键细节）----
//
//  迭代时只扫 all 里**当前元素数最少**的那个类型的稠密数组，
//  其余条件用位图 O(1) 探测。
//  不这么做的话，"1 万个 Transform 里只有 3 个 Agent"这种查询
//  会白扫 1 万次 —— 症状是"加了个组件之后帧率掉了"，
//  而且怎么 profiling 都指向错的地方。
//  选择结果会写进 query，调试时能直接看到（ecs_query Driving type）。
//
//  ---- 迭代器看到的是一致快照 ----
//
//  开始时快照驱动类型的元素数，所以迭代过程中**新加**的实体本轮不会被看到
//  （它们下一轮才被看见）。而迭代过程中**删组件**之所以危险，
//  是因为交换式移除会移动数组元素，让后续行号指错 ——
//  那才是必须延后的原因，world 的 active_iterator_count 会把它变成断言。
// =============================================================================
#ifndef ENGINE_ECS_QUERY_H
#define ENGINE_ECS_QUERY_H

#include "engine/core/types.h"
#include "engine/ecs/component.h"
#include "engine/ecs/entity.h"
#include "engine/ecs/world.h"

#define ECS_MAX_QUERY_TERMS 6u

typedef struct ecs_query_desc_t {
    ecs_component_id all[ECS_MAX_QUERY_TERMS];
    u32              all_count;
    ecs_component_id none[ECS_MAX_QUERY_TERMS];
    u32              none_count;
    const char      *debug_name;
} ecs_query_desc_t;

// 常用的便捷构造：1~3 个 all 条件。写全参数版本只是为了让
// "查询要哪些组件"在调用点一眼可见，不要靠记忆填数组。
ecs_query_desc_t ecs_query_desc_1(ecs_component_id a, const char *name);
ecs_query_desc_t
ecs_query_desc_2(ecs_component_id a, ecs_component_id b, const char *name);
ecs_query_desc_t
ecs_query_desc_3(ecs_component_id a, ecs_component_id b, ecs_component_id c, const char *name);

typedef struct ecs_query_t ecs_query_t;

// 创建查询。会校验 all/none 有重叠（同一类型既要求又有就是不可能的）
// 与未注册的类型 —— 这两种都在创建时就报错，而不是等运行时静默不命中。
// 返回 NULL 表示描述非法或内存不足。
ecs_query_t *ecs_query_create(ecs_world_t *world, const ecs_query_desc_t *desc);
void         ecs_query_destroy(ecs_world_t *world, ecs_query_t *query);

// 当前命中数（近似值：等于驱动类型的元素数，未扣除 none 与交叉条件）。
// 用来**选择驱动类型**与在统计里看查询形状，不是精确计数。
u32 ecs_query_estimate_count(const ecs_query_t *query);
// 精确命中数（扫一遍驱动数组并逐个验条件）。只在调试/统计里用。
u32 ecs_query_match_count(const ecs_query_t *query);

// 选中的驱动类型（调试用：判断"为什么这个查询慢"第一眼就看它）。
ecs_component_id ecs_query_driving_type(const ecs_query_t *query);
const char      *ecs_query_debug_name(const ecs_query_t *query);

// ---------------------------------------------------------------- 迭代
typedef struct ecs_iter_t {
    // 非 const：列指针是**可写的**（系统在组件里写结果就是这个用途），
    // 所以迭代器本身也必须能被用来登记/解除迭代计数。
    struct ecs_world_t *world;
    const ecs_query_t  *query;

    entity_t entity;                    // 当前行对应的实体
    usize    index;                     // 已经产出多少个命中（0 起）
    u32      driving_row;               // 驱动数组里的当前位置

    u32      term_count;                // = all_count
    void    *columns[ECS_MAX_QUERY_TERMS]; // 与 desc.all 同序；none 不出现在这里
    u32      skipped;                   // 本次迭代中被条件挡掉的行数（诊断用）
} ecs_iter_t;

// it 由调用方栈上分配。begin 会登记"有迭代器在跑"，end 必须配对调用，
// 否则世界的迭代计数会一直不归零，之后所有立即版增删都会断言失败 ——
// 这是故意的：宁可吵，也不要静默地让迭代器踩到移动过的数组。
void ecs_query_begin(ecs_query_t *query, ecs_iter_t *it);
// 推进到下一个命中。返回 false 表示结束。
bool ecs_query_next(ecs_iter_t *it);
void ecs_query_end(ecs_iter_t *it);

// 取第 i 个 all 条件的组件指针（列下标与 desc.all 一致）。
#define ECS_COLUMN(iter, i, T) ((T *)(iter)->columns[i])
// 等价写法：按组件 id 找列（找不到返回 NULL，用于条件分支少的场合）。
void *ecs_iter_column(const ecs_iter_t *it, ecs_component_id id);

// 语法糖这里**故意不做**。begin/next/end 三段式一旦被 for 宏包起来，
// "漏掉 end"的成因（迭代计数不归零）就被宏的括号结构藏住，
// 而调用点上再也看不见这条需要成对调用的规则。
//
// 忘了 end 由世界兜底：ecs_world_update 结束时检查
// active_iterator_count == 0，不对就明确报错。
// 宁可吵一声，也不要变成"之后所有立即版增删随机断言失败"这种没线索的现象。

#endif // ENGINE_ECS_QUERY_H
