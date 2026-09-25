// =============================================================================
//  src/ecs/ecs_internal.h —— ECS 各实现文件共享的内部布局
//
//  这个头**不装 include/engine 里**：世界结构的具体布局是实现细节，
//  对外只暴露 ecs_world_t 的不完整类型。放这里的唯一理由是
//  五个 .c 文件都要看得到成员，而把它们各自复制一份必然漂移。
// =============================================================================
#ifndef ENGINE_ECS_INTERNAL_H
#define ENGINE_ECS_INTERNAL_H

#include "engine/core/array.h"
#include "engine/core/memory.h"
#include "engine/ecs/component.h"
#include "engine/ecs/entity.h"
#include "engine/ecs/query.h"
#include "engine/ecs/system.h"
#include "engine/ecs/transform.h"
#include "engine/ecs/world.h"

// 深度缓存里表示"还没算过"的值。
#define ECS_DEPTH_UNKNOWN 0xFFFFFFFFu

// ---------------------------------------------------------------- 组件存储
//
// 每类型一份稠密数组。**不用 pool_t**：这里要的是"按行号连续扫"的
// 迭代局部性，而 pool 的空闲槽会把数组打散，还会带来一层
// 我们并不需要的代际检查（代际由实体句柄负责，组件行不需要）。
typedef struct ecs_storage_t {
    // **世界自己持有描述表的副本**。早先这里存的是"调用方 desc 的指针"，
    // 于是 ecs_register_builtin_components 里那个为了覆盖 construct 而
    // 造的栈上局部副本，在函数返回后就变成了悬垂指针 ——
    // 之后每次按名字查组件都对死栈帧做 strcmp，读到一个被当成指针的
    // 垃圾值（ASan: access-violation at 0x45e0360e）。
    // 存副本把这一整类"注册表借用调用方内存"的坑一次性堵掉。
    ecs_component_desc_t  desc_storage;
    const ecs_component_desc_t *desc; // 指向上面那份副本，让 s->desc->x 的写法不变
    // 注意：desc_storage.fields 仍然是**借来的指针**。字段数组必须是
    // 静态生命周期的（所有真实用法都是文件级 static const 表）。
    // 世界不知道也不该知道它的生命周期 —— 所以这条写进 component.h 的约定。
    bool                        registered;

    void  *data; // capacity * stride
    u32    count;
    u32    capacity;
    u32    stride;

    entity_t *owners; // 行号 -> 实体句柄（存完整句柄而不是 index：
                      // 迭代时要还给调用方的就是句柄，index 还得再查一次代际）
    u32      *row_of; // handle_index(entity) -> 行号；仅当 presence 位为 1 时有效
    // row_of / presence 的**元素个数**（= 句柄 index 的取值空间，
    // 比 capacity 大 1 且与实体容量相关，不是同一个数）。
    // 记下来是为了释放时能给出和分配时完全一致的字节数 ——
    // 追踪分配器会核对 free 的 size，两者不等会当场断言。
    u32       index_space;
    u64      *presence;
    usize     presence_words;

    u32 add_failures; // 容量不足：用来把 capacity 从"猜"变成"量"
} ecs_storage_t;

// ---------------------------------------------------------------- 实体记录
typedef struct ecs_entity_record_t {
    dense_id_t dense_id; // 由 ecs_rebuild_dense_index 维护
    u32        reserved;
} ecs_entity_record_t;

// ---------------------------------------------------------------- 命令
typedef enum ecs_command_kind_t {
    ECS_CMD_DESTROY_ENTITY = 1, // 摘掉该实体的所有组件并归还身份槽
    ECS_CMD_ADD_COMPONENT,
    ECS_CMD_REMOVE_COMPONENT,
    ECS_CMD_SET_PARENT,
    ECS_CMD_KIND_COUNT
} ecs_command_kind_t;

typedef struct ecs_command_t {
    u8              kind; // ecs_command_kind_t
    u8              flags;   // bit0：payload 有效
    u16             reserved;
    entity_t        a; // 主体（SET_PARENT 时是 child）
    entity_t        b; // SET_PARENT 的父；其余为 ENTITY_NONE
    ecs_component_id type;
    u32              payload_size;
    u8               payload[ECS_COMMAND_PAYLOAD_CAPACITY];
} ecs_command_t;

// ---------------------------------------------------------------- 系统
struct ecs_system_t {
    ecs_system_desc_t desc;
    bool              registered;
    bool              enabled; // 关掉但不注销：调试时按个开关很常用
    u32               run_count;
};

// ---------------------------------------------------------------- 查询
struct ecs_query_t {
    ecs_world_t *world;
    ecs_query_desc_t desc; // 复制：列顺序以它为准，调用方的 desc 可能已是栈变量

    ecs_component_id driving; // all 里当前基数最小的
    u32              term_count;

    u32 estimate; // 创建/刷新时驱动类型的元素数快照
    u32 matches;  // 上一次精确统计
    u32 probes;   // 上一次扫过的行数 —— 和 matches 一比就知道有没有白扫
};

// ---------------------------------------------------------------- 世界
struct ecs_world_t {
    bool ready;
    ecs_world_config_t config;

    pool_t entities;     // ecs_entity_record_t
    u32    entity_peak;

    ecs_storage_t storage[ECS_MAX_COMPONENTS];
    u32           type_count;

    u32 active_iterators;

    // ---- 层级 ----
    entity_t *topo;      // 拓扑序：存完整句柄（不是 index），因为取组件要过代际检查
    u32       topo_count;
    u32       topo_capacity;
    u32   *depth_cache;  // handle_index -> 深度；ECS_DEPTH_UNKNOWN 表示还没算
    bool   hierarchy_dirty;
    u32    hierarchy_rebuilds;
    u32    hierarchy_cycles_rejected;
    u32    hierarchy_depth_max_seen;

    // ---- dense id ----
    dense_id_t next_dense_id;
    entity_t  *dense_to_entity; // 长度 entity_capacity
    u32        dense_count;
    bool       dense_dirty;

    // ---- 系统 ----
    ecs_system_t systems[ECS_MAX_SYSTEMS];
    u32          system_count; // 已按 (stage, priority, name) 排好
    u32          systems_run_last_update;

    // ---- 命令 ----
    array_t commands; // ecs_command_t
    u32     commands_flushed;
    u32     command_overflows;

    usize memory_bytes;
};

// ---------------------------------------------------------------- 内部工具
// 读路径拿 const 存储，写路径拿可写存储。
// 只有一个返回非 const 的 ecs_storage_of 的话，所有 const 入参的函数
// 都得靠强转穿透自己定的规则 —— 那 const 就成了装饰。
const ecs_storage_t *ecs_storage_of(const ecs_world_t *world, ecs_component_id id);
ecs_storage_t       *ecs_storage_of_mut(ecs_world_t *world, ecs_component_id id);

static inline bool ecs_presence_test(const ecs_storage_t *s, u32 index)
{
    return (s->presence[index >> 6] >> (index & 63u)) & 1u;
}

static inline void ecs_presence_set(ecs_storage_t *s, u32 index, bool value)
{
    const usize word = index >> 6;
    const u64   bit = (u64)1 << (index & 63u);
    if (value) {
        s->presence[word] |= bit;
    } else {
        s->presence[word] &= ~bit;
    }
}

// 迭代器/系统会碰到的内部入口
void ecs_apply_command(const ecs_command_t *cmd, ecs_world_t *world);
bool ecs_add_immediate(ecs_world_t *world, entity_t entity, ecs_component_id id,
                       const void *data);
bool ecs_remove_immediate(ecs_world_t *world, entity_t entity, ecs_component_id id);
bool ecs_destroy_immediate(ecs_world_t *world, entity_t entity);

#endif // ENGINE_ECS_INTERNAL_H
