// =============================================================================
//  engine/ecs/world.h —— 世界：实体身份、组件存储、迭代计数、统计
//
//  ---- 一个世界 = 一套完整的实体/组件状态 ----
//
//  世界**不是全局单例**（引擎总目标是可多实例）：它由调用方提供存储，
//  可以同时存在多个（例如编辑器里的"当前场景"与"预览场景"）。
//  但同一个世界不是线程安全的 —— M5 不做并行调度，这是明确的不做。
//
//  ---- 谁 owns 什么 ----
//
//  world owns：实体池、每个组件类型的存储、描述表自身的**副本**、
//  命令队列、拓扑序数组。
//  world 不 owns 的只有 desc->fields 那张字段数组：它必须是静态生命周期的
//  （所有真实用法都是文件级 static const 表）。
//  为什么副本而不是借指针：注册内置组件时要覆盖 construct，于是造了一个
//  栈上的 desc 局部 —— 借指针的做法让它一返回就悬垂，之后每次按名字查组件
//  都在对死栈帧做 strcmp。复制 desc 把这类坑一次性堵掉。
//
//  ---- active_iterator_count ----
//
//  世界自己记着"现在有几个迭代器在跑"。这是组件增删"立即版"的安全阀：
//  Debug 下带迭代器调用立即版会 ASSERT 失败，而不是留下一个偶发的内存问题。
// =============================================================================
#ifndef ENGINE_ECS_WORLD_H
#define ENGINE_ECS_WORLD_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/ecs/component.h"
#include "engine/ecs/entity.h"

struct ecs_system_t;

typedef struct ecs_world_t ecs_world_t;

typedef struct ecs_world_config_t {
    allocator_t *allocator; // 所有内部存储的来源

    // 实体数上限。注意它同时决定每个组件类型的 row_of 映射大小
    // （entity_capacity * 4 字节 * 类型数），所以不是免费的。
    u32 entity_capacity;

    // 注册组件时没写 capacity 就用这个默认值。
    u32 default_component_capacity;

    // 一个 stage 边界能积压多少条延后命令。超了不会踩内存，
    // 会计数并丢弃 + 记错误 —— 丢命令的表现是"某个实体没被创建"，
    // 比踩内存好定位得多。
    u32 command_capacity;

    // 父子链的最大深度。设了硬上限，编辑器里手滑把父节点拖到自己的
    // 子节点下面时，会在设置的那一刻报错，而不是递归到栈溢出。
    u32 hierarchy_depth_max;

    const char *name; // 给日志与内存统计用
} ecs_world_config_t;

// 填一份保守默认：16k 实体、组件默认 4k 槽、命令 8k、层级 32。
ecs_world_config_t ecs_world_config_default(allocator_t *allocator);

// 创建世界。与 renderer_create 同一种约定：**不透明指针 + 调用方给分配器**，
// 世界的内部布局（几十 KB 的存储表）不进公共头，改实现不重编上层。
// 返回 NULL 表示参数非法或分配失败。
ecs_world_t *ecs_world_create(allocator_t *allocator, const ecs_world_config_t *config);
void         ecs_world_destroy(ecs_world_t *world);
bool         ecs_world_is_valid(const ecs_world_t *world);

// ---------------------------------------------------------------- 实体
//
// 创建立即生效：它只占一个身份槽，不碰任何组件存储，
// 所以不可能让正在跑的迭代器失效。
entity_t ecs_create_entity(ecs_world_t *world);

// 销毁必须延后：它要把实体从所有组件类型里摘掉，那会移动池元素。
// 在系统外（没有迭代器时）调用会立即执行；在系统里调用会排进命令队列。
bool ecs_destroy_entity(ecs_world_t *world, entity_t entity);

bool ecs_entity_is_alive(const ecs_world_t *world, entity_t entity);

// 清空所有实体与组件，但**保留**已注册的组件类型（重载场景用）。
void ecs_world_clear(ecs_world_t *world);

// ---------------------------------------------------------------- 迭代保护
u32  ecs_active_iterator_count(const ecs_world_t *world);
void ecs_world_acquire_iterator(ecs_world_t *world);
void ecs_world_release_iterator(ecs_world_t *world);

// ---------------------------------------------------------------- dense id
//
// 只为序列化服务：运行时一律用 entity_t。
// dense id 从 0 开始紧凑编号，顺序 = 创建顺序（在压平数组里）。
dense_id_t ecs_entity_to_dense(const ecs_world_t *world, entity_t entity);
// 入参是非 const：dense -> 句柄的映射是惰性重建的，而重建会写世界。
// 想要纯查询语义就自己先调 ecs_rebuild_dense_index，再读缓存。
entity_t ecs_entity_from_dense(ecs_world_t *world, dense_id_t dense);
// 重建映射（世界内容变化后调用；世界内部在 flush 时自己维护）。
void ecs_rebuild_dense_index(ecs_world_t *world);

// 把存活实体按 dense id **升序**填进 out，返回填了多少个。out 为 NULL 时只返回总数
// （用来先探规模再分配）。max 不够时按 max 截断并记错误 —— 静默丢掉实体是最糟的，
// 存档会"看起来成功但少了几十个物体"。
//
// 为什么专门要这一个：存档与"存→读→再存逐字节一致"这条验收要求写出顺序
// 与内存布局无关。池的迭代顺序是槽位顺序，组件数组的行顺序在交换式移除后
// 也不稳定。dense id 是唯一两边都同意、且跨运行可复现的序。
u32 ecs_collect_entities(ecs_world_t *world, entity_t *out, u32 max);

// ---------------------------------------------------------------- 统计
typedef struct ecs_world_stats_t {
    u32 entity_capacity;
    u32 entities_alive;
    u32 entity_peak;

    u32 component_types;
    u32 component_instances;   // 所有类型元素数之和
    u32 component_add_failures; // 容量不够的次数 —— 用来校准容量
    u32 component_instance_capacity;

    u32 commands_flushed;
    u32 command_overflows;

    u32 systems_registered;
    u32 systems_run_last_update;

    u32 hierarchy_depth_max_seen;
    u32 hierarchy_rebuilds;
    u32 hierarchy_rejected_cycles;

    usize memory_bytes; // 粗略：各存储的分配量之和
} ecs_world_stats_t;

void ecs_world_get_stats(const ecs_world_t *world, ecs_world_stats_t *out_stats);

// 把统计打到日志（性能回归与"容量是量出来的"都靠它）。
void ecs_world_log_stats(const ecs_world_t *world);

#endif // ENGINE_ECS_WORLD_H
