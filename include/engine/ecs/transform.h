// =============================================================================
//  engine/ecs/transform.h —— 变换与父子关系（ECS 内置组件）
//
//  ---- 为什么放在 ECS 层而不是场景层 ----
//
//  只依赖 core 的数学类型，不碰渲染，所以属于 ECS。
//  而"网格 + 材质 + 图层"那类组件要看 render 的头文件，放在 scene 层 ——
//  这条界线就是 M5 约束 3（ECS 不 include render）的落点。
//
//  ---- 2D 与 3D 共用同一份 transform ----
//
//  2D 常用像素、z 当层内排序键；3D 用世界单位。看起来该分两套，
//  但分了的后果是 M6 的混合渲染要在两套之间搬运数据，
//  而"同一份场景里 2D 精灵挂在 3D 节点下"这种需求立刻变成特例。
//  差异体现在**提交时**（选哪个 layer、用哪个相机的投影），不在存储上。
//
//  ---- 世界变换用拍平拓扑序，不用递归（决策 D4）----
//
//  递归求值代码最短，但递归深度由数据决定：编辑器里手滑拖出一条
//  10 万层的链，或者一个环，就是把栈打穿。
//  这里改成：先按深度把实体排成拓扑序（父一定在子前面），
//  然后顺序扫一遍。层级变化时标脏、重建一次。
//  代价是多一个数组和一次重建；换来的是**没有递归**、且顺序确定。
// =============================================================================
#ifndef ENGINE_ECS_TRANSFORM_H
#define ENGINE_ECS_TRANSFORM_H

#include "engine/core/mat.h"
#include "engine/core/quat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/ecs/component.h"
#include "engine/ecs/entity.h"

struct ecs_world_t;

// 本地变换（相对父节点；没有父节点就是相对世界原点）。
// 用结构体而不是 mat4：位置/旋转/缩放分开存，插值与编辑器编辑都方便，
// 而且 60 字节比 64 字节的矩阵紧凑。世界矩阵是算出来的，不在这存。
typedef struct ecs_transform_t {
    vec3_t position;
    quat_t rotation;
    vec3_t scale;
} ecs_transform_t;

// 算出来的世界变换。系统每帧写，渲染读。
// 单独一个组件而不是塞进 transform：只读的派生数据混进可写的源数据里，
// 迟早出现"改了本地值但世界值还是旧的"这种一致性坑。
typedef struct ecs_world_transform_t {
    mat4_t model;         // 世界矩阵
    mat4_t normal_matrix; // (model^-1)^T —— 非均匀缩放下变换法线必须用它
} ecs_world_transform_t;

// 父子关系。单独一个组件，这样"没有父节点"不需要存一个 ENTITY_NONE 占位，
// 层级查询也能用 has(parent) 直接判。
typedef struct ecs_parent_t {
    entity_t parent;
} ecs_parent_t;

// 内置组件的 id 打包。注册一次，之后按名字查（不记数字）。
typedef struct ecs_builtin_components_t {
    ecs_component_id transform;
    ecs_component_id world_transform;
    ecs_component_id parent;
} ecs_builtin_components_t;

// 在世界上注册这三个组件。重复调用是幂等的（已注册就返回现有 id）。
// 返回 false 表示注册失败（名字冲突或类型表满）。
//
// transform 的 construct 会把 scale 初始化为 1 而不是全零 ——
// 全零缩放让法线矩阵求逆失败，表现是整个模型黑掉，
// 看起来完全像光照 bug，几乎没人会往"默认值"上想。
bool ecs_register_builtin_components(struct ecs_world_t *world,
                                     ecs_builtin_components_t *out_ids);

// ---------------------------------------------------------------- 便捷操作
//
// 取/设本地变换（没有 transform 组件就加上）。
ecs_transform_t *ecs_transform_get_or_add(struct ecs_world_t *world, entity_t entity);
bool             ecs_transform_set_local(struct ecs_world_t *world, entity_t entity,
                                         vec3_t position, quat_t rotation, vec3_t scale);
bool ecs_transform_get_world(const struct ecs_world_t *world, entity_t entity,
                             ecs_world_transform_t *out);

// 立即设置父子（会做环与深度检查）。系统里请用 ecs_defer_set_parent。
// parent = ENTITY_NONE 解除关系。返回 false 并记错误：成环、超深度、
// 实体不存在、自己当自己的父。
bool ecs_set_parent(struct ecs_world_t *world, entity_t child, entity_t parent);
entity_t ecs_get_parent(const struct ecs_world_t *world, entity_t child);
// 当前父链深度（根节点算 0）。没有 parent 组件返回 0。
u32 ecs_hierarchy_depth(const struct ecs_world_t *world, entity_t entity);

// 层级脏标记（改了父子关系后调用；ecs_set_parent / 命令队列都会自动标）。
void ecs_mark_hierarchy_dirty(struct ecs_world_t *world);
// 重建拓扑序。由 ecs_world_update 在 TRANSFORM 之前自动调用；
// 公开是为了让测试与"加载完立刻要世界矩阵"的场合能手动触发。
bool ecs_rebuild_hierarchy(struct ecs_world_t *world);

// 跑一遍世界矩阵求值（按拓扑序）。通常由 TRANSFORM 阶段自动调用。
void ecs_update_world_transforms(struct ecs_world_t *world);

// 拓扑序里的实体数（= 有 transform 组件的实体数）。
u32 ecs_hierarchy_node_count(const struct ecs_world_t *world);
// 实测到的最大父链深度，用来校准 hierarchy_depth_max 是否设得太小。
u32 ecs_hierarchy_depth_max_seen(const struct ecs_world_t *world);

#endif // ENGINE_ECS_TRANSFORM_H
