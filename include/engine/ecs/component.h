// =============================================================================
//  engine/ecs/component.h —— 组件类型注册、字段描述表、增删查
//
//  ---- 存储模型：per-type 稠密数组 + 存在性位图（设计决策 D1-A）----
//
//  每个组件类型一份独立存储：
//      data    : count 个元素紧密排列（同类型遍历 = 顺序扫内存）
//      owners  : data[i] 属于哪个实体（迭代时要知道实体身份）
//      row_of  : entity index -> data 下标，O(1) 反查
//      presence: 位图，query 里"这个实体有没有该组件"是 O(1) 且只看一个字
//
//  删除用**交换式移除**（把最后一个搬到被删的位置），因此
//  "同一个类型内实体的相对顺序"在删过之后不保证稳定 ——
//  需要确定性的地方（存档、编辑器显示）一律按 dense id 重排，
//  别依赖迭代顺序。
//
//  ---- 为什么不用 archetype ----
//
//  archetype（按组件组合分表）能让多类型 join 也保持连续，代价是
//  加/删组件要把实体在表之间搬、还要维护表图。M5 不背这个复杂度。
//  升级线写死在 docs/M5：多类型 join 实测超过单帧预算 5% 才考虑。
//
//  ---- 字段描述表是本阶段的承重设计（决策 D3）----
//
//  C 没有真反射，所以序列化、编辑器属性面板、prefab 需要的"这个组件有哪些
//  字段"信息必须手写一张表。有了它，二进制读写 / JSON 读写 / 属性面板
//  是同一份代码；否则每加一个组件要手写四五个序列化函数，必漏。
//
//  代价：表和结构体会漂移。防线两道：
//    1. 每个内置组件用 _Static_assert + offsetof 把 offset/size 钉死；
//    2. 存档头写 schema 哈希，读的时候不匹配就明确拒绝 ——
//       宁可拒绝一个不认识的存档，也不要"读出一半还报告成功"。
// =============================================================================
#ifndef ENGINE_ECS_COMPONENT_H
#define ENGINE_ECS_COMPONENT_H

#include "engine/core/types.h"
#include "engine/ecs/entity.h"

struct ecs_world_t;

// 最多支持的组件类型数。每注册一个类型会多出 entity_capacity * 4 字节的
// row_of 映射，所以这个数字**不是免费的**：32 个类型 x 65536 实体 = 8 MB。
// 容量是量出来的不是猜的 —— world 的统计里能看到实际占用。
#define ECS_MAX_COMPONENTS 32u

typedef u32 ecs_component_id;
#define ECS_COMPONENT_NONE ((ecs_component_id)0xFFFFFFFFu)

#define ECS_COMPONENT_NAME_MAX 32u

// ---------------------------------------------------------------- 字段描述
typedef enum ecs_field_kind_t {
    ECS_FIELD_F32 = 0, // f32
    ECS_FIELD_F64,     // f64
    ECS_FIELD_U32,     // u32
    ECS_FIELD_I32,     // i32
    ECS_FIELD_U8,      // u8
    ECS_FIELD_BOOL,    // bool
    ECS_FIELD_VEC2,    // vec2_t
    ECS_FIELD_VEC3,    // vec3_t
    ECS_FIELD_VEC4,    // vec4_t
    ECS_FIELD_QUAT,    // quat_t
    ECS_FIELD_ENTITY,  // entity_t：序列化成 dense id，加载时重映射
    ECS_FIELD_DENSE_ID, // dense_id_t：允许指向场景外（读回时映射不到就是 NONE）
    ECS_FIELD_STRING,   // 内联 char[N]，需要 capacity

    // 资源引用：内存里是 scene 资源表的 u32 下标，**文件里是名字**。
    //
    // 为什么不落索引：手改存档时在资源列表中间插一行，后面所有索引就全体错位，
    // 而且**读得出来** —— 只是"这个模型穿了那个模型的皮"。
    // 存名字让人手编辑仍然安全，代价只是加载时多一次按名字查表。
    //
    // 这三个种类是"给 ECS 用的序列化知识"，看着像越层，但反过来更糟：
    // 若让场景层自己认字段名来决定"这个 u32 要不要翻译成名字"，
    // 就等于在 ECS 之外复制一份字段表 —— 两份表必然漂移。
    ECS_FIELD_MESH_REF,
    ECS_FIELD_MATERIAL_REF,
    ECS_FIELD_ATLAS_REF,
    ECS_FIELD_KIND_COUNT
} ecs_field_kind_t;

// 单个字段的描述。
//
// count > 1 表示定长数组（元素类型由 kind 决定，步长就是该类型的自然大小）。
// 不支持任意嵌套与动态数组 —— 那是"把反射做成通用系统"的坡，滑下去就没有底。
// 需要复合结构就把它注册成独立组件，或者拆成多个字段。
typedef struct ecs_field_desc_t {
    const char      *name;
    u16              offset;
    ecs_field_kind_t kind;
    u16              count;    // 0 由注册时补成 1
    u16              capacity; // STRING 的字节容量（含结尾 NUL）
    u16              reserved;
} ecs_field_desc_t;

// 字段种类对应的字节数；未知返回 0。
usize ecs_field_kind_size(ecs_field_kind_t kind);
const char *ecs_field_kind_name(ecs_field_kind_t kind);

// 这个字段一共占多少字节（数组按 count 乘、字符串按 capacity）。
//
// count == 0 是"1 个元素"的简写，**只在读取侧解释**：描述表通常是
// 文件级 static const，写进只读段之后强转 const 去补全是未定义行为。
usize ecs_field_byte_size(const ecs_field_desc_t *field);

// ---------------------------------------------------------------- 组件描述
typedef struct ecs_component_desc_t {
    const char *name; // 存档里靠名字对账，不靠 id
    u32         size; // sizeof(组件结构体)
    u32         align;

    // 该类型的槽位上限。pool 不扩容，所以必须注册时给够；
    // 分配失败的次数会被统计进 world，用来反过来校准这个数。
    u32 capacity;

    const ecs_field_desc_t *fields;
    u32                     field_count;

    // 该组件是**派生数据**，不进存档。
    // 必须有这个显式标记，因为"零字段"有两种完全不同的含义：
    //   - 标记组件（Tag）：没有数据，但"它有"这件事本身要存下来；
    //   - 派生组件（世界矩阵）：存了会在下次加载时被重算覆盖，白占空间
    //     还会造成"存档里的值和屏幕上不一致"的假象。
    // 用 field_count==0 同时表达这两层意思，早晚要撞。
    bool skip_serialize;

    // 新实体加上这个组件时填默认值。允许为 NULL（全零即可的组件）。
    void (*construct)(void *component);

    // 反序列化之后修一次：典型用途是把 dense id 引用换成真正的 entity_t。
    // 返回 false 表示这条数据不可用（整个加载失败）。允许为 NULL。
    bool (*post_load)(void *component, struct ecs_world_t *world, entity_t self);
} ecs_component_desc_t;

// ---------------------------------------------------------------- 注册
//
// 按名字查已注册的类型 id；没注册过返回 ECS_COMPONENT_NONE。
// 组件 id 由注册顺序决定，所以**注册顺序本身是初始化契约的一部分**：
// 两个模块抢注同名组件会被拒绝（而不是各发一个 id，那会让存档彻底错位）。
ecs_component_id ecs_register_component(struct ecs_world_t *world,
                                        const ecs_component_desc_t *desc);
ecs_component_id ecs_find_component(const struct ecs_world_t *world, const char *name);
bool             ecs_component_is_registered(const struct ecs_world_t *world,
                                             ecs_component_id id);
const ecs_component_desc_t *ecs_component_desc(const struct ecs_world_t *world,
                                               ecs_component_id id);

// 当前已注册的组件类型数。
u32 ecs_component_type_count(const struct ecs_world_t *world);

// 该类型的 schema 哈希（由名字 + 字段名/偏移/种类/数量算出）。
// 存档头里写它，读的时候比对 —— 结构改了但忘了改存档版本，
// 症状会从"读出一堆乱值"变成一行明确的错误。
u32 ecs_component_schema_hash(const ecs_component_desc_t *desc);

// ---------------------------------------------------------------- 增删查
//
// ---- 关于"什么时候可以立即改" ----
//
// 迭代过程中移动池元素会毁掉正在跑的迭代器，这是 ECS 的头号坑。
// 规则做成**机器可检**的，而不是靠人记住：
//   * 立即版（本节的 add/remove/destroy）在没有活跃迭代器时才允许，
//     Debug 下会 ASSERT 检查。场景加载就是走这条：加载时没有系统在跑。
//   * 系统里必须用 defer 版（见 system.h），它们在 stage 边界统一落地。
// 之所以保留立即版而不是一刀切只给 defer：10k 实体的场景加载如果每一步
// 都要排命令、再排空，代码会既慢又难读；而"加载期没有迭代器"这个前提
// 恰好是可以被断言验证的。

// 加组件。data 为 NULL 时按 construct（没有则全零）初始化。
// 已经存在则用 data 覆盖其值并返回原指针。
void *ecs_add(struct ecs_world_t *world, entity_t entity, ecs_component_id id, const void *data);

// 取组件。const 世界只能拿到只读指针，非 const 世界才能拿到可写指针 ——
// 分成两个函数而不是"一个 ecs_get 返回 void* 再让 const 世界也调用"：
// 后者要么从 const 对象里递出可写内存（等于把 const 当装饰），
// 要么逼调用方到处加强转，两种都把"谁能改"这条信息丢掉了。
const void *ecs_peek(const struct ecs_world_t *world, entity_t entity, ecs_component_id id);
void       *ecs_get(struct ecs_world_t *world, entity_t entity, ecs_component_id id);

bool ecs_has(const struct ecs_world_t *world, entity_t entity, ecs_component_id id);

// 移除组件。返回是否真的移除了（没有就是 false）。
bool ecs_remove(struct ecs_world_t *world, entity_t entity, ecs_component_id id);

// ---- 两个多数组件用得上的糖 ----
// 取，没有就加后再取。
void *ecs_get_or_add(struct ecs_world_t *world, entity_t entity, ecs_component_id id,
                     const void *data);
// 取，且要求可写；调用方保证它存在（不存在返回 NULL 并记错误）。
void *ecs_edit(struct ecs_world_t *world, entity_t entity, ecs_component_id id);

u32 ecs_alive_entity_count(const struct ecs_world_t *world);
u32 ecs_component_count(const struct ecs_world_t *world, ecs_component_id id);
u32 ecs_component_capacity(const struct ecs_world_t *world, ecs_component_id id);

// 遍历某个类型的全部持有者（不调用任何系统，仅用于统计/存档/编辑器）。
// 顺序是内存顺序，**不保证稳定**（见文件头的交换式移除）。
//
// 结构体是具体的、可以栈上分配：这里不藏指针，因为"迭代器到底持有什么"
// 直接影响它失效的条件，摆在头文件里比藏进 .c 更容易看明白。
typedef struct ecs_type_iter_t {
    const struct ecs_world_t *world;
    ecs_component_id          id;
    u32                       index;    // 下一个要取的行号
    u32                       count;    // 该类型当前元素数（开始时快照）
    const void               *data;     // 组件数组基址（只读：这个遍历器服务于
    const entity_t           *owners;   // 存档/统计，拿可写指针请走 query）
    u32                       stride;
} ecs_type_iter_t;

void ecs_type_iter_begin(const struct ecs_world_t *world, ecs_component_id id,
                         ecs_type_iter_t *out_it);
// out_component 允许为 NULL（只想要实体）。
bool ecs_type_iter_next(ecs_type_iter_t *it, entity_t *out_entity, const void **out_component);

usize ecs_type_iter_remaining(const ecs_type_iter_t *it);

#endif // ENGINE_ECS_COMPONENT_H
