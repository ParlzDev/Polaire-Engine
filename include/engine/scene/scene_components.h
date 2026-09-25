// =============================================================================
//  engine/scene/scene_components.h —— 面向渲染的组件
//
//  ---- 为什么这一层在 scene/ 而不是 ecs/ ----
//
//  M5 约束 3：ECS 层不 include 任何 render 头。这些组件要引用
//  mesh/material/texture/layer 等渲染资源，所以放在场景层。
//  于是依赖方向是单向的：scene -> ecs + render，谁都不反过来依赖谁。
//  好处很具体：ECS 可以被物理、脚本、编辑器头表复用，而不必拖着 GL 走。
//
//  ---- 组件里存的是**表索引**，不是 GPU 句柄 ----
//
//  句柄（mesh_handle_t / texture_handle_t）是"这一次运行"的产物：
//  重启就变，热重载也会变。把它们写进存档，下次读出来指向的是别的资源。
//  所以组件存 scene 资源表里的下标，**存档时再翻译成名字**。
//  这一层翻译同时解决了另一件事：场景文件里写 "cube.mesh" 比写 0x1a2b
//  可读得多，M11 的编辑器也要靠名字显示。
//
//  ---- 每个组件都配一张字段描述表 + 编译期偏移断言 ----
//
//  C 没有真反射。序列化、属性面板全靠那张表，而表和结构体之间的对应
//  只能靠 _Static_assert 钉 —— 改了结构体忘了改表，症状是"存档读出一堆
//  乱值且不报错"，那是最难查的一类问题。
// =============================================================================
#ifndef ENGINE_SCENE_SCENE_COMPONENTS_H
#define ENGINE_SCENE_SCENE_COMPONENTS_H

#include "engine/core/mat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/ecs/component.h"
#include "engine/render/render_queue.h" // render_layer_t

#define SCENE_NAME_MAX 32u

// ---------------------------------------------------------------- 网格渲染器
typedef struct scene_mesh_renderer_t {
    u32  mesh;        // scene 资源表下标
    u32  material;    // scene 资源表下标
    u32  layer;       // render_layer_t（存 u32 以免序列化要理解枚举宽度）
    bool cast_shadow; // false = 不进阴影深度 pass（线框/参考网格必须关掉）
} scene_mesh_renderer_t;

_Static_assert(sizeof(scene_mesh_renderer_t) <= 64u, "mesh_renderer 超出命令内联负载上限");
_Static_assert(ENGINE_OFFSETOF(scene_mesh_renderer_t, mesh) == 0, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_mesh_renderer_t, material) == 4, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_mesh_renderer_t, layer) == 8, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_mesh_renderer_t, cast_shadow) == 12, "字段表漂移");

static const ecs_field_desc_t k_mesh_renderer_fields[] = {
    { "mesh", 0u, ECS_FIELD_U32, 1u, 0u, 0u },
    { "material", 4u, ECS_FIELD_U32, 1u, 0u, 0u },
    { "layer", 8u, ECS_FIELD_U32, 1u, 0u, 0u },
    { "cast_shadow", 12u, ECS_FIELD_BOOL, 1u, 0u, 0u },
};

static const ecs_component_desc_t scene_mesh_renderer_desc = {
    .name = "MeshRenderer",
    .size = sizeof(scene_mesh_renderer_t),
    .align = 16u,
    .fields = k_mesh_renderer_fields,
    .field_count = (u32)(sizeof(k_mesh_renderer_fields) / sizeof(k_mesh_renderer_fields[0])),
};

// ---------------------------------------------------------------- 精灵渲染器
//
// 位置与旋转来自 Transform（同一份 transform，2D/3D 共用 —— 这是刻意的，
// 分两套的话 M6 的"2D 精灵挂在 3D 节点下"立刻变成特例）。
// 这里只放精灵特有的东西。
// 成员按对齐**从大到小**排：vec4 要 16 对齐，把 u32 放在它前面就会
// 让编译器插 12 字节填充，而字段表里写死的偏移必须和真实布局一致 ——
// 排好序让两者都没有意外。
typedef struct scene_sprite_renderer_t {
    vec4_t color;
    vec2_t size;
    vec2_t uv_min;
    vec2_t uv_max;
    u32    atlas; // scene 资源表下标；SCENE_RESOURCE_NONE = 用纯色
    u32    layer;
} scene_sprite_renderer_t;

_Static_assert(sizeof(scene_sprite_renderer_t) == 48u, "sprite_renderer 尺寸变了");
_Static_assert(ENGINE_OFFSETOF(scene_sprite_renderer_t, color) == 0, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_sprite_renderer_t, size) == 16, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_sprite_renderer_t, uv_min) == 24, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_sprite_renderer_t, uv_max) == 32, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_sprite_renderer_t, atlas) == 40, "字段表漂移");
_Static_assert(ENGINE_OFFSETOF(scene_sprite_renderer_t, layer) == 44, "字段表漂移");

static const ecs_field_desc_t k_sprite_renderer_fields[] = {
    { "color", 0u, ECS_FIELD_VEC4, 1u, 0u, 0u },
    { "size", 16u, ECS_FIELD_VEC2, 1u, 0u, 0u },
    { "uv_min", 24u, ECS_FIELD_VEC2, 1u, 0u, 0u },
    { "uv_max", 32u, ECS_FIELD_VEC2, 1u, 0u, 0u },
    { "atlas", 40u, ECS_FIELD_U32, 1u, 0u, 0u },
    { "layer", 44u, ECS_FIELD_U32, 1u, 0u, 0u },
};

static const ecs_component_desc_t scene_sprite_renderer_desc = {
    .name = "SpriteRenderer",
    .size = sizeof(scene_sprite_renderer_t),
    .align = 16u,
    .fields = k_sprite_renderer_fields,
    .field_count = (u32)(sizeof(k_sprite_renderer_fields) / sizeof(k_sprite_renderer_fields[0])),
};

// ---------------------------------------------------------------- 相机
typedef enum scene_camera_projection_t {
    SCENE_CAMERA_PERSPECTIVE = 0,
    SCENE_CAMERA_ORTHOGRAPHIC = 1,
} scene_camera_projection_t;

typedef struct scene_camera_t {
    u32  projection;   // scene_camera_projection_t
    f32  fov_y_degrees; // 竖直视角（度）。存档里用度而不是弧度：手改文件时更符合直觉
    f32  ortho_height;
    f32  znear;
    f32  zfar;
    bool active; // 一帧只用一个相机；同时有多个 active 时取实体 id 最小的那个
} scene_camera_t;

static const ecs_field_desc_t k_camera_fields[] = {
    { "projection", (u16)ENGINE_OFFSETOF(scene_camera_t, projection), ECS_FIELD_U32, 1u, 0u, 0u },
    { "fov_y_degrees", (u16)ENGINE_OFFSETOF(scene_camera_t, fov_y_degrees), ECS_FIELD_F32, 1u, 0u,
      0u },
    { "ortho_height", (u16)ENGINE_OFFSETOF(scene_camera_t, ortho_height), ECS_FIELD_F32, 1u, 0u,
      0u },
    { "znear", (u16)ENGINE_OFFSETOF(scene_camera_t, znear), ECS_FIELD_F32, 1u, 0u, 0u },
    { "zfar", (u16)ENGINE_OFFSETOF(scene_camera_t, zfar), ECS_FIELD_F32, 1u, 0u, 0u },
    { "active", (u16)ENGINE_OFFSETOF(scene_camera_t, active), ECS_FIELD_BOOL, 1u, 0u, 0u },
};

static void scene_camera_construct(void *component)
{
    scene_camera_t *c = (scene_camera_t *)component;
    c->projection = (u32)SCENE_CAMERA_PERSPECTIVE;
    c->fov_y_degrees = 60.0f;
    c->ortho_height = 10.0f;
    c->znear = 0.1f;
    c->zfar = 200.0f;
    c->active = true;
}

static const ecs_component_desc_t scene_camera_desc = {
    .name = "Camera",
    .size = sizeof(scene_camera_t),
    .align = 16u,
    .fields = k_camera_fields,
    .field_count = (u32)(sizeof(k_camera_fields) / sizeof(k_camera_fields[0])),
    .construct = scene_camera_construct,
};

// ---------------------------------------------------------------- 光源
//
// 点光/聚光的位置取自实体的世界变换 —— 所以挂在父子链上就能"提着灯走"，
// 不需要在组件里再存一份坐标（存两份迟早会不一致）。
typedef struct scene_light_t {
    u32    type; // 0 平行光 / 1 点光 / 2 聚光（与 light_type_t 同序）
    vec3_t color;
    f32    intensity;
    vec3_t direction; // 平行光/聚光用；**指向被照物体**
    f32    range;
    f32    inner_angle_degrees;
    f32    outer_angle_degrees;
    bool   cast_shadow;
} scene_light_t;

static const ecs_field_desc_t k_light_fields[] = {
    { "type", (u16)ENGINE_OFFSETOF(scene_light_t, type), ECS_FIELD_U32, 1u, 0u, 0u },
    { "color", (u16)ENGINE_OFFSETOF(scene_light_t, color), ECS_FIELD_VEC3, 1u, 0u, 0u },
    { "intensity", (u16)ENGINE_OFFSETOF(scene_light_t, intensity), ECS_FIELD_F32, 1u, 0u, 0u },
    { "direction", (u16)ENGINE_OFFSETOF(scene_light_t, direction), ECS_FIELD_VEC3, 1u, 0u, 0u },
    { "range", (u16)ENGINE_OFFSETOF(scene_light_t, range), ECS_FIELD_F32, 1u, 0u, 0u },
    { "inner_angle_degrees", (u16)ENGINE_OFFSETOF(scene_light_t, inner_angle_degrees),
      ECS_FIELD_F32, 1u, 0u, 0u },
    { "outer_angle_degrees", (u16)ENGINE_OFFSETOF(scene_light_t, outer_angle_degrees),
      ECS_FIELD_F32, 1u, 0u, 0u },
    { "cast_shadow", (u16)ENGINE_OFFSETOF(scene_light_t, cast_shadow), ECS_FIELD_BOOL, 1u, 0u, 0u },
};

static void scene_light_construct(void *component)
{
    scene_light_t *l = (scene_light_t *)component;
    l->type = 0u;
    l->color = vec3_splat(1.0f);
    l->intensity = 1.0f;
    l->direction = vec3_make(0.0f, -1.0f, 0.0f);
    // range 默认给一个有限值而不是 0：半径 0 的点光什么都照不到，
    // 而"灯没亮"看起来像 shader bug。
    l->range = 20.0f;
    l->inner_angle_degrees = 25.0f;
    l->outer_angle_degrees = 35.0f;
    l->cast_shadow = false;
}

static const ecs_component_desc_t scene_light_desc = {
    .name = "Light",
    .size = sizeof(scene_light_t),
    .align = 16u,
    .fields = k_light_fields,
    .field_count = (u32)(sizeof(k_light_fields) / sizeof(k_light_fields[0])),
    .construct = scene_light_construct,
};

// ---------------------------------------------------------------- 名字
typedef struct scene_name_t {
    char text[SCENE_NAME_MAX];
} scene_name_t;

static const ecs_field_desc_t k_name_fields[] = {
    { "text", 0u, ECS_FIELD_STRING, 0u, SCENE_NAME_MAX, 0u },
};

static const ecs_component_desc_t scene_name_desc = {
    .name = "Name",
    .size = sizeof(scene_name_t),
    .align = 16u,
    .fields = k_name_fields,
    .field_count = (u32)(sizeof(k_name_fields) / sizeof(k_name_fields[0])),
};

// ---------------------------------------------------------------- 资源表下标
//
// 组件里的 mesh/material/atlas 字段用这个值表示"没有资源"。
// 用 0 而不是 NONE 是不行的：0 是一个合法的下标。
#define SCENE_RESOURCE_NONE 0xFFFFFFFFu

// 场景里注册的所有组件类型 id 的打包。加载/提交/编辑器都靠它查表。
typedef struct scene_component_ids_t {
    ecs_component_id transform;
    ecs_component_id world_transform;
    ecs_component_id parent;
    ecs_component_id mesh_renderer;
    ecs_component_id sprite_renderer;
    ecs_component_id camera;
    ecs_component_id light;
    ecs_component_id name;
} scene_component_ids_t;

#endif // ENGINE_SCENE_SCENE_COMPONENTS_H
