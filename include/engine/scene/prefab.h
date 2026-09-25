// =============================================================================
//  engine/scene/prefab.h —— 预制体：可复用的实体子树
//
//  ---- 预制体就是"一段带根的存档字节" ----
//
//  这里没有第二套格式，也没有第二套校验：prefab_t 持有的是 scene_write_prefab
//  产出的字节副本，实例化走的就是 scene_read_binary_root。
//
//  为什么不另设一套内存结构（组件数组 + 实体数组）：那要有两份"什么叫合法"，
//  而两份一定会漂 —— 今天查的一个格式 bug 就是同一个长度算了两遍的结果。
//  为什么不复制成"每个实例一份组件数据"：字节已经是紧凑的了，
//  解码反而是唯一需要理解字段表的地方。
//
//  ---- 校验发生在用的那一刻 ----
//
//  prefab_create / prefab_load 都不检查内容。看着像"太懒"，但换个角度：
//  预制体唯一被消费的地方就是实例化，而实例化必须完整读一遍才知道好不好 ——
//  在那之前先检查一遍，得到的是同一条错误信息的两次机会，
//  以及"加载时没报错、实例化时报错"这种说不清的状态。
//  所以坏文件的第一条错误日志来自 prefab_instantiate，那里也正是它该被看到的地方。
//
//  ---- 实例化 N 次的代价 ----
//
//  每次实例化 = 一次完整解码：O(子树实体数 × 组件数)。资源表按名字去重，
//  所以只有**第一次**会往表里加条目，之后都是查表。
//  真正要留意的是场景的 scratch arena：每次解码都要一张按 dense 编号的临时映射表，
//  所以读档入口一进一出各做一次 mark/rewind（见 scene_io.c）。
//  少了那次 rewind，"实例化 100 次"就是 100 × 实体容量 × 4 字节的净增长，
//  而且前 99 次都不会失败。
//
//  ---- 同名资源的规则 ----
//
//  实例化时如果目标场景已经有同名资源，**先用到的那份生效**（资源表按名字去重）。
//  表现是"这个预制体在我这儿贴图不对"，实际是场景里先注册了同名材质。
//  这里不覆盖也不报错：覆盖会让两个实体的引用互相影响，
//  而报错会让"用同一个材质名"这种正常用法变成加载失败。
//  真要区分，就该用不同的名字 —— 名字在这里就是身份的判据。
// =============================================================================
#ifndef ENGINE_SCENE_PREFAB_H
#define ENGINE_SCENE_PREFAB_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/ecs/entity.h"
#include "engine/scene/scene.h"
#include "engine/scene/scene_io.h"

typedef struct prefab_t prefab_t;

// ---------------------------------------------------------------- 造

// 从场景里的一个子树（root 及其后代）做一份预制体。
// alloc 是**预制体自己的**分配器（它持有字节的副本），可以和场景用的不是同一个。
// 失败返回 NULL（root 无效、子树为空、字节复制时分配失败）。
prefab_t *prefab_from_subtree(scene_t *scene, entity_t root, allocator_t *alloc,
                              scene_io_stats_t *out_stats);

// 从内存字节做。会复制一份，所以调用方的 arena 可以立刻继续用或整体丢掉。
prefab_t *prefab_create(allocator_t *alloc, const void *bytes, usize size);

// 从文件做。文件多大就分配多大，不预先假设上限 —— 上限在读档时被检查（那里才有
// "实体数 / 资源表容量"这些真正的判据）。
prefab_t *prefab_load(allocator_t *alloc, const char *path);

void prefab_destroy(prefab_t *prefab);

// ---------------------------------------------------------------- 用
//
// parent 无效（ENTITY_NONE）= 作为根实体挂在目标场景顶层；有效 = 挂到它下面，
// 于是预制体的根成为它的一个子节点，本地变换仍然是存档里那份（相对父节点）。
//
// 目标场景可以和做出这份预制体的场景**不是同一个** —— 只要组件注册表一致。
// 不一致时实例化会失败并说明是注册表哈希不符，这是预制体唯一的兼容性检查，
// 而且它和"旧存档"用的是同一道门。
bool prefab_instantiate(const prefab_t *prefab, scene_t *target, entity_t parent,
                        entity_t *out_root, scene_io_stats_t *out_stats);

// ---------------------------------------------------------------- 查

usize         prefab_byte_size(const prefab_t *prefab);
// 这份预制体会造出多少个实体 = 实例化一次的实体开销。0 表示 prefab 为 NULL。
// 数字是从头部**读**出来的，所以它对一份坏文件也可能给出一个荒谬值 ——
// 用它做判断的调用方（比如"要不要实例化"）仍然要以 instantiate 的返回值为准。
u32           prefab_entity_count(const prefab_t *prefab);

#endif // ENGINE_SCENE_PREFAB_H
