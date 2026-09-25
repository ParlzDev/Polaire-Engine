// =============================================================================
//  src/scene/scene_io_internal.h —— 两种编码共用的一张表
//
//  场景存档有二进制和 JSON 两种形式。它们**必须**在"什么算同一个字段"上不可能
//  分歧，所以字段表、记录结构、以及"长度可以纯从表算出来"这条规则只有一份，
//  放在这里，由 scene_io.c 与 scene_json.c 各自 include。
//
//  为什么不是"各写一份表"：本阶段查出来的格式 bug 全部来自同一个形态 ——
//  同一个长度算两遍、两遍不一样。表抄两份是同一类错误的放大版，而且这次连
//  长度自检都救不了（两边各自自洽，只是互相不认识）。
//
//  为什么是 static const 而不是 extern：项目里字段表本来就是这个写法
//  （见 scene_components.h 的 k_mesh_renderer_fields）。每个编译单元各有一份
//  只读副本，代价是几百字节 rodata；换来的是"表在头文件里就能看全"，
//  以及不必为一组常量发明访问函数。
// =============================================================================
#ifndef ENGINE_SCENE_IO_INTERNAL_H
#define ENGINE_SCENE_IO_INTERNAL_H

#include <string.h>

#include "engine/core/byte_io.h"
#include "engine/core/log.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/ecs/component.h"
#include "engine/render/material.h" // RENDER_MAX_TEXTURE_SLOTS
#include "engine/scene/scene.h"

// ---------------------------------------------------------------- 记录结构
//
// 环境、资源条目、材质都不是组件，但形状一样（一块内存 + 一张字段表），
// 所以三处共用同一套编解码。

typedef struct scene_io_resource_t {
    char name[SCENE_RESOURCE_NAME_MAX];
    char path[SCENE_RESOURCE_NAME_MAX * 2u]; // 空 = 由应用提供的程序化几何
} scene_io_resource_t;

typedef struct scene_io_material_t {
    char name[SCENE_RESOURCE_NAME_MAX];
    char pipeline[SCENE_RESOURCE_NAME_MAX];
    // 每个槽位一个图集名（空串 = 该槽位没挂）。
    //
    // 8 槽 × 64 字节 = 512 字节/材质，材质表上限 64 → 二进制里最坏多占 32KB。
    // 定长的好处是"这一条记录仍然被那一套定长遍历处理"：变长就要加第二套读法,
    // 而"跳过不认识的组件"依赖的正是尺寸可纯从表算出。
    // JSON 形式里这一串会打成数组并且省掉尾部的空串 —— 人手编辑时不用数空格。
    char atlases[RENDER_MAX_TEXTURE_SLOTS][SCENE_RESOURCE_NAME_MAX];
    vec4_t tint;
    vec4_t pbr;
    vec4_t emissive;
} scene_io_material_t;

// ---------------------------------------------------------------- 字段表

#define IO_FIELD(name_str, type, field, kind_val)                                                  \
    {                                                                                              \
        name_str, (u16)ENGINE_OFFSETOF(type, field), kind_val, 1u, 0u, 0u                          \
    }
// 定长字符串字段。capacity 是**一个元素**的字节容量，count 是元素个数 ——
// 两者的乘积才是线上尺寸。
//
// 为什么 count 要显式写出来而不是靠 capacity 的数值去猜（"512 一定是 8 个名字"）：
// 两种编码都得知道这是一串名字还是一个长字符串，而靠长度反推语义意味着
// "改一个名字上限"会静默改变字段的含义。
#define IO_FIELD_STR(name_str, type, field)                                                        \
    {                                                                                              \
        name_str, (u16)ENGINE_OFFSETOF(type, field), ECS_FIELD_STRING, 1u,                          \
            (u16)sizeof(((type *)0)->field), 0u                                                    \
    }
#define IO_FIELD_STR_N(name_str, type, field, n)                                                   \
    {                                                                                              \
        name_str, (u16)ENGINE_OFFSETOF(type, field), ECS_FIELD_STRING, (u16)(n),                    \
            (u16)(sizeof(((type *)0)->field) / (n)), 0u                                            \
    }

static const ecs_field_desc_t k_env_fields[] = {
    IO_FIELD("clear_color", scene_env_t, clear_color, ECS_FIELD_VEC4),
    IO_FIELD("clear_color_enabled", scene_env_t, clear_color_enabled, ECS_FIELD_BOOL),
    IO_FIELD("clear_depth_enabled", scene_env_t, clear_depth_enabled, ECS_FIELD_BOOL),
    IO_FIELD("ambient_color", scene_env_t, ambient_color, ECS_FIELD_VEC3),
    IO_FIELD("ambient_intensity", scene_env_t, ambient_intensity, ECS_FIELD_F32),
    IO_FIELD("fog_enabled", scene_env_t, fog_enabled, ECS_FIELD_BOOL),
    IO_FIELD("fog_color", scene_env_t, fog_color, ECS_FIELD_VEC3),
    IO_FIELD("fog_start", scene_env_t, fog_start, ECS_FIELD_F32),
    IO_FIELD("fog_end", scene_env_t, fog_end, ECS_FIELD_F32),
    IO_FIELD("fog_density", scene_env_t, fog_density, ECS_FIELD_F32),
    IO_FIELD("skybox_enabled", scene_env_t, skybox_enabled, ECS_FIELD_BOOL),
    IO_FIELD("sky_horizon", scene_env_t, sky_horizon, ECS_FIELD_VEC3),
    IO_FIELD("sky_zenith", scene_env_t, sky_zenith, ECS_FIELD_VEC3),
    IO_FIELD("sky_ground", scene_env_t, sky_ground, ECS_FIELD_VEC3),
    IO_FIELD("tonemap_mode", scene_env_t, tonemap_mode, ECS_FIELD_U32),
    IO_FIELD("exposure", scene_env_t, exposure, ECS_FIELD_F32),
    IO_FIELD("msaa_samples", scene_env_t, msaa_samples, ECS_FIELD_U32),
    IO_FIELD("shadows_enabled", scene_env_t, shadows_enabled, ECS_FIELD_BOOL),
    IO_FIELD("shadow_strength", scene_env_t, shadow_strength, ECS_FIELD_F32),
    IO_FIELD("shadow_map_size", scene_env_t, shadow_map_size, ECS_FIELD_U32),
    IO_FIELD("culling_enabled", scene_env_t, culling_enabled, ECS_FIELD_BOOL),
    IO_FIELD("use_scene_target", scene_env_t, use_scene_target, ECS_FIELD_BOOL),
};

static const ecs_component_desc_t k_env_desc = {
    .name = "Env",
    .size = sizeof(scene_env_t),
    .align = 16u,
    .fields = k_env_fields,
    .field_count = (u32)(sizeof(k_env_fields) / sizeof(k_env_fields[0])),
};

static const ecs_field_desc_t k_resource_fields[] = {
    IO_FIELD_STR("name", scene_io_resource_t, name),
    IO_FIELD_STR("path", scene_io_resource_t, path),
};

static const ecs_component_desc_t k_resource_desc = {
    .name = "Resource",
    .size = sizeof(scene_io_resource_t),
    .align = 16u,
    .fields = k_resource_fields,
    .field_count = (u32)(sizeof(k_resource_fields) / sizeof(k_resource_fields[0])),
};

static const ecs_field_desc_t k_material_fields[] = {
    IO_FIELD_STR("name", scene_io_material_t, name),
    IO_FIELD_STR("pipeline", scene_io_material_t, pipeline),
    // 8 个槽位 = 8 个定长名字，所以 count 写出来。二进制里仍然是一整块 512 字节,
    // JSON 里打成 8 个元素的数组 —— 同一张表，两种渲染，语义不分叉。
    IO_FIELD_STR_N("atlases", scene_io_material_t, atlases, RENDER_MAX_TEXTURE_SLOTS),
    IO_FIELD("tint", scene_io_material_t, tint, ECS_FIELD_VEC4),
    IO_FIELD("pbr", scene_io_material_t, pbr, ECS_FIELD_VEC4),
    IO_FIELD("emissive", scene_io_material_t, emissive, ECS_FIELD_VEC4),
};

static const ecs_component_desc_t k_material_desc = {
    .name = "Material",
    .size = sizeof(scene_io_material_t),
    .align = 16u,
    .fields = k_material_fields,
    .field_count = (u32)(sizeof(k_material_fields) / sizeof(k_material_fields[0])),
};

// ---------------------------------------------------------------- 尺寸规则

// 一种字段在**文件里**占多少字节（两种编码共用这条规则：JSON 里对应的是一行的
// 元素个数，二进制里是字节数，而"跳过不认识的组件"靠的就是它能纯从表算出）。
//
// 和内存大小不等的只有 vec3：内存里 16（含对齐用的 pad），文件里 12。
// 这两个数必须分清 —— 组件的"每段长度前缀"是从这里算的，要是拿内存大小去算，
// 声明的长度比实际写出的多 4，读侧就会以为下个组件从那里开始，于是全体错位。
static inline usize scene_io_field_elem_wire_size(ecs_field_kind_t kind)
{
    return (kind == ECS_FIELD_VEC3) ? 3u * sizeof(f32) : ecs_field_kind_size(kind);
}

static inline usize scene_io_field_wire_size(const ecs_field_desc_t *f)
{
    const usize count = (f->count > 0u) ? (usize)f->count : 1u;
    if (f->kind == ECS_FIELD_STRING) {
        return (usize)f->capacity * count;
    }
    if (f->kind == ECS_FIELD_MESH_REF || f->kind == ECS_FIELD_MATERIAL_REF ||
        f->kind == ECS_FIELD_ATLAS_REF) {
        return (usize)SCENE_RESOURCE_NAME_MAX; // 落名字，一个引用一个名字
    }
    return scene_io_field_elem_wire_size(f->kind) * count;
}

static inline usize scene_io_desc_wire_size(const ecs_component_desc_t *desc)
{
    usize total = 0u;
    for (u32 i = 0; i < desc->field_count; ++i) {
        total += scene_io_field_wire_size(&desc->fields[i]);
    }
    return total;
}

// 材质那条记录里"图集"字段的槽位数（两种编码都从这儿数，不写第二份常量）。
#define SCENE_IO_MATERIAL_SLOTS RENDER_MAX_TEXTURE_SLOTS

// 这份注册表里哪些类型要进存档（世界变换是派生数据，永远不进）。
//
// 两种编码共用**同一个顺序**：JSON 导入时也要按这个顺序摆它的类型表，
// 否则"二进制 → JSON → 二进制"会在类型表那一段和每条记录的类型下标上错位，
// 而那份字节相同正是"两种编码描述同一份数据"唯一的直接证据。
u32 scene_serializable_types(const ecs_world_t *world, ecs_component_id *out, u32 max);

// 一份存档里"实体段"的单文件上限。两种编码共用：JSON 形式如果不受同一个数限制，
// 就会出现"这个 JSON 能存但转不成存档字节"的第二种失败模式。
#define SCENE_IO_MAX_ENTITIES_PER_FILE 65536u

// ---------------------------------------------------------------- 默认记录
//
// JSON 允许**省略字段**（人手编辑时只写关心的那几个），省略的取值来自
// 这里给的默认记录 —— 和组件的 construct 走同一条路，于是"JSON 里没写的字段"
// 与"代码里新加的组件"永远是同一个值。
//
// 为什么默认值必须是"整条记录"而不是"逐字段查表"：逐字段的话需要一张
// 字段名 → 默认值的表，那正是抄两份就会漂移的东西（而且它和 construct 重复）。

// 材质记录的默认值：三个 vec4 取 material_default()，名字与槽位取空。
static inline void scene_io_material_record_default(scene_io_material_t *out)
{
    const material_t mat = material_default();
    memset(out, 0, sizeof(*out));
    out->tint = mat.tint;
    out->pbr = mat.pbr_params;
    out->emissive = mat.emissive;
}

// ---------------------------------------------------------------- 长度自检
//
// "字段表算出的长度 == 真写出去的字节数"这条不变量由两种编码各自核对，
// 但**报错的措辞和判定只有一份**：两条路上各写一遍的话，其中一条早晚会退化成
// "只在函数末尾比总长"，而那正是本阶段最难查的一次 bug 的形状（差几个字节，
// 差在谁身上没人知道）。
static inline bool scene_io_check_record_bytes(const ecs_component_desc_t *desc, usize want,
                                               usize emitted)
{
    if (emitted == want) {
        return true;
    }
    LOG_ERROR("%s 的长度不符：字段表算出 %llu 字节，实际写出 %llu 字节", desc->name,
              (unsigned long long)want, (unsigned long long)emitted);
    return false;
}

// ---------------------------------------------------------------- 存档内核
//
// 两种编码**共用**的存档内核，都在 scene_io.c 里实现：
//
// * JSON 的两个方向都是**纯转码** —— 二进制 → JSON 文本、JSON → 二进制，
//   中间不读也不写场景状态。之所以可能：二进制格式自己就是自描述的
//   （资源落名字、实体引用落 dense id），所以转码不需要"句柄→索引→名字"
//   这类必须有活的场景才能做的翻译。
// * 于是"哪些实体进存档、按什么顺序、带哪些资源"这条策略**只有一处实现**
//   （scene_io_write_archive），两种编码不可能在它上面分叉。
// * 这两个函数都不做 scratch arena 的 mark/rewind：缓冲区归调用方的 arena，
//   谁给 arena 谁负责丢。公开的 scene_*_binary / scene_*_json 入口负责那一步。
bool scene_io_write_archive(scene_t *scene, const entity_t *scope, u32 scope_count,
                            arena_t *arena, void **out_bytes, usize *out_size,
                            scene_io_stats_t *out_stats);
bool scene_io_read_archive(scene_t *scene, const void *bytes, usize size,
                           scene_io_stats_t *out_stats, entity_t *out_root);

// 落盘与读盘。同样是"两种编码共用"：短写、fclose 失败、空文件、超出 scratch
// 这些错误处理抄两遍，迟早有一遍是错的。
bool scene_io_write_file(const void *bytes, usize size, const char *path, const char *what);
// 把整个文件读进 arena（不 mark/rewind —— 调用方负责），out_size 不含 NUL。
bool scene_io_read_file(const char *path, arena_t *arena, void **out_buf, usize *out_size);

#endif // ENGINE_SCENE_IO_INTERNAL_H
