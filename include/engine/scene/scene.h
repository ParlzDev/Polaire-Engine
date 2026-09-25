// =============================================================================
//  engine/scene/scene.h —— 场景 = 世界 + 资源表 + 环境参数
//
//  ---- 为什么资源表在场景里，而不是让组件直接拿 GPU 句柄 ----
//
//  组件里存句柄的话，存档就完了：句柄是"这一次运行"的产物，
//  重启、换后端、热重载都会变。所以场景持有一张**按名字索引**的资源表，
//  组件存表下标，存档存名字。三层各自稳定：
//      存档   → 名字（人可读、跨运行不变）
//      运行期 → 表下标（一次加载内不变）
//      GPU    → 句柄（随时可换）
//  热重载（M7）要做的正是"换掉第三层而前两层不动"。
//
//  ---- 管线为什么由应用注册进来 ----
//
//  pipeline 是"着色器 + 一整套渲染状态"的组合，属于程序逻辑而不是场景数据：
//  让场景文件去创建管线，等于把 shader 源文件路径写进关卡表里。
//  所以这里只存**名字到句柄的映射**，应用负责造管线并按名字登记；
//  场景里引用一个没登记的名字会在加载时报错并跳过，而不是静默用错管线
//  （"画面全黑但日志干净"是这类问题最难查的形态）。
// =============================================================================
#ifndef ENGINE_SCENE_SCENE_H
#define ENGINE_SCENE_SCENE_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/ecs/world.h"
#include "engine/render/material.h"
#include "engine/render/renderer.h"
#include "engine/render/render_types.h"
#include "engine/scene/scene_components.h"

#define SCENE_MAX_MESHES 128u
#define SCENE_MAX_ATLASES 64u
#define SCENE_MAX_MATERIALS 64u
#define SCENE_MAX_PIPELINES 32u
#define SCENE_RESOURCE_NAME_MAX 64u

typedef struct scene_t scene_t;

// 环境参数：不属于任何实体、但决定画面长相的那一堆。
// 全部是标量/颜色，所以序列化是直给的。
typedef struct scene_env_t {
    vec4_t clear_color;
    bool   clear_color_enabled;
    bool   clear_depth_enabled;

    vec3_t ambient_color;
    f32    ambient_intensity;

    bool   fog_enabled;
    vec3_t fog_color;
    f32    fog_start;
    f32    fog_end;
    f32    fog_density;

    bool   skybox_enabled;
    vec3_t sky_horizon;
    vec3_t sky_zenith;
    vec3_t sky_ground;

    u32  tonemap_mode; // tonemap_mode_t
    f32  exposure;
    u32  msaa_samples;

    bool   shadows_enabled;
    f32    shadow_strength;
    u32    shadow_map_size;
    bool   culling_enabled;
    bool   use_scene_target; // 离屏 + 后处理路径的总开关
} scene_env_t;

// 保守但能看的一套默认值（与 M4 的 05_model 对齐）。
scene_env_t scene_env_default(void);

typedef struct scene_config_t {
    allocator_t *allocator;
    // 为 NULL 时是**离线模式**：只登记资源名字与下标，不能从文件加载。
    // 序列化与 prefab 的单元测试走这条路 —— 它们要验的是
    // "存出去读回来逐字节一致"，不需要真的把画面画出来。
    renderer_t *renderer;
    const char  *name;
    u32          entity_capacity;      // 0 = 用 ecs 默认
    u32          resource_capacity_per_type; // 每个组件类型的槽位；0 = 默认
} scene_config_t;

scene_t *scene_create(const scene_config_t *config);
void     scene_destroy(scene_t *scene);

// 场景自带的 scratch arena：存档缓冲、加载期的 dense 映射表这类
// "用完就整体丢掉"的临时内存都走这里，所以它们不需要各自的分配/释放配对，
// 也就不会有"某条错误路径忘了 free"这种泄漏。
// 与 renderer_frame_arena 的区别：那个每帧 reset，这个跨帧存活但不能长命 ——
// 序列化可能一次就要几百 KB，塞进帧 arena 会把帧预算带偏。
arena_t *scene_scratch_arena(scene_t *scene);

// 注册场景用到的全部组件类型（ECS 内置的三个 + 五个渲染向的）。
// 幂等：重复调用只是取回同一批 id。06_scene、测试、prefab 都先调它。
bool scene_register_components(scene_t *scene, scene_component_ids_t *out_ids);

ecs_world_t *scene_world(scene_t *scene);
renderer_t  *scene_renderer(scene_t *scene);
const scene_component_ids_t *scene_components(const scene_t *scene);
scene_env_t *scene_environment(scene_t *scene);
void         scene_set_environment(scene_t *scene, const scene_env_t *env);

// ---------------------------------------------------------------- 管线
bool scene_register_pipeline(scene_t *scene, const char *name, pipeline_handle_t pipeline);
bool scene_lookup_pipeline(const scene_t *scene, const char *name, pipeline_handle_t *out);
u32  scene_pipeline_count(const scene_t *scene);
const char *scene_pipeline_name_at(const scene_t *scene, u32 index);

// ---------------------------------------------------------------- 资源表
//
// 每个 add 都按名字去重：同一张图/同一个网格被两个实体引用时，
// 只应该有一份 GPU 资源。返回的下标稳定，可以直接写进组件。

// 从文件加载网格（.mesh 优先，.obj 回落）。path 决定实际走哪条解析路径。
bool scene_add_mesh_from_file(scene_t *scene, const char *name, const char *path, u32 *out_id);
// 用已有句柄登记一个网格（程序化几何走这条路，避免为了测试硬造文件）。
bool scene_add_mesh_handle(scene_t *scene, const char *name, mesh_handle_t mesh, u32 *out_id);

// 从文件加载纹理/图集（PNG/TGA 等由 stb_image 解码，sRGB 由后端决定）。
bool scene_add_atlas_from_file(scene_t *scene, const char *name, const char *path, u32 *out_id);
bool scene_add_atlas_handle(scene_t *scene, const char *name, texture_handle_t texture,
                            u32 *out_id);

// 材质：pipeline 用**名字**引用，在这里就解析成句柄；解析不到直接失败，
// 不等渲染时才出现"用了默认管线"这种说不清的状态。
bool scene_add_material(scene_t *scene, const char *name, const char *pipeline_name,
                        const material_t *material, u32 *out_id);
// 给材质挂一张已登记的图集（按槽位）。
bool scene_material_set_atlas(scene_t *scene, u32 material_id, u32 slot, u32 atlas_id);

u32  scene_find_mesh(const scene_t *scene, const char *name);
u32  scene_find_atlas(const scene_t *scene, const char *name);
u32  scene_find_material(const scene_t *scene, const char *name);
const char *scene_mesh_name(const scene_t *scene, u32 id);
const char *scene_mesh_path(const scene_t *scene, u32 id);
const char *scene_atlas_name(const scene_t *scene, u32 id);
const char *scene_material_name(const scene_t *scene, u32 id);
// 资源表的两条"来源"信息，序列化用：从文件来的记 path，程序化几何 path 为空。
// 管线索引同理 —— 存档里必须是名字，句柄跨运行/跨后端都会变。
const char *scene_atlas_path(const scene_t *scene, u32 id);
const char *scene_material_pipeline(const scene_t *scene, u32 id);

mesh_handle_t   scene_mesh_handle(const scene_t *scene, u32 id);
texture_handle_t scene_atlas_handle(const scene_t *scene, u32 id);
// 取材质的**已解析**副本（pipeline 已填好）。找不到返回 NULL。
const material_t *scene_material(const scene_t *scene, u32 id);

u32 scene_mesh_count(const scene_t *scene);
u32 scene_atlas_count(const scene_t *scene);
u32 scene_material_count(const scene_t *scene);

// 下标的无符号"越界即无效"检查（存档里读到的 id 是外部输入，必须验）
bool scene_mesh_id_valid(const scene_t *scene, u32 id);
bool scene_atlas_id_valid(const scene_t *scene, u32 id);
bool scene_material_id_valid(const scene_t *scene, u32 id);

#endif // ENGINE_SCENE_SCENE_H
