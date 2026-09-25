// =============================================================================
//  examples/06_scene/main.c —— M5 演示：数据驱动场景、预制体、ECS 阶段调度
//
//  这个示例的**默认路径里没有实体清单**。所有实体、材质、灯光、环境参数都来自
//  assets/scenes/m5_demo.scene。加载之后代码只做四件事：
//      1. 造渲染资源（管线、程序化纹理），按名字绑进场景的资源表；
//      2. 注册项目自己的组件类型（Spin）和系统；
//      3. 每帧跑一次世界 + 提交；
//      4. 打印统计。
//
//  author_demo_scene() 只在"存档不存在"或 --save 时跑，它存在的唯一理由是把那份
//  .scene 造出来（格式是二进制的，手写字节流不现实）。M11 的编辑器会取代它。
//
//  验证 M5 的哪几条：
//    * 实体 + 组件：网格/精灵/相机/灯/名字 + 一个项目级组件 Spin
//    * 固定阶段调度：Spin 在 LOGIC 阶段，世界矩阵在 TRANSFORM 阶段，同一帧内成立
//    * 父子层级：轨道相机只是"会转的节点的子节点"，没有一行特例代码
//    * 场景可存读：.scene；往返逐字节一致由 tests/test_scene_io.c 钉住
//    * 预制体：.prefab 可反复实例化，资源依赖跟着走
//    * ECS→渲染桥接：画面里的一切都经 scene_submit_frame
//    * 1k/10k/100k 实体的帧时间与内存：--bench N
//
//  操作：
//      ESC   退出
//      N     加一根柱子（实例化一次预制体）
//      P     把当前场景存回 .scene（关掉程序再开就能看到数据驱动的效果）
//      R     清空世界，重新从 .scene 加载
//
//  命令行：
//      --scene PATH      场景存档路径（默认 assets/scenes/m5_demo.scene）
//      --prefab PATH     预制体路径（默认 assets/scenes/m5_pillar.prefab）
//      --save            重新生成两份资产后退出
//      --frames N        跑 N 帧后退出（CI 用）
//      --bench N         建 N 个实体，测 world_update / 提交的 CPU 耗时后退出
//      --instances N     启动后立刻实例化 N 份预制体并计时
//      --hidden / --no-vsync / --width / --height
// =============================================================================
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"

// ecs/ 与 scene/ 刻意不在 engine.h 里（理由见那份注释），示例自己按需包含。
#include "engine/ecs/ecs.h"
#include "engine/scene/ecs_submit.h"
#include "engine/scene/prefab.h"
#include "engine/scene/scene.h"
#include "engine/scene/scene_io.h"

#define DEMO_MAX_BENCH_ENTS 200000u
#define DEMO_BENCH_FRAMES 30u
#define DEMO_PILLAR_LIMIT 64u
#define DEMO_MESH_PATH "assets/models/cube.mesh"

// =============================================================================
//  项目级组件：Spin
//
//  引擎不认识"自转"。这一节演示的是正交扩展点：注册一个组件 + 一个系统，
//  不改引擎任何一行。字段表要手写并配编译期偏移断言（C 没有反射），
//  因为"改了结构体忘了改表"的症状是存档读出一堆乱值且不报错。
// =============================================================================
typedef struct demo_spin_t {
    f32 degrees_per_second;
    u32 axis; // 0 = X，1 = Y，2 = Z
} demo_spin_t;

_Static_assert(ENGINE_OFFSETOF(demo_spin_t, degrees_per_second) == 0, "spin 字段表漂移");
_Static_assert(ENGINE_OFFSETOF(demo_spin_t, axis) == 4, "spin 字段表漂移");

static const ecs_field_desc_t k_spin_fields[] = {
    { "degrees_per_second", 0u, ECS_FIELD_F32, 1u, 0u, 0u },
    { "axis", 4u, ECS_FIELD_U32, 1u, 0u, 0u },
};

static const ecs_component_desc_t k_spin_desc = {
    .name = "Spin",
    .size = sizeof(demo_spin_t),
    .align = 16u,
    .capacity = 0u, // 0 = 用世界的默认容量
    .fields = k_spin_fields,
    .field_count = (u32)(sizeof(k_spin_fields) / sizeof(k_spin_fields[0])),
};

typedef struct demo_t {
    scene_t              *scene;
    scene_component_ids_t ids;
    ecs_component_id      spin;
    ecs_query_t          *spin_query;
    u32                   spins_applied; // 上一帧被系统改过的组件数：证明系统在跑
    u32                   last_spins;
} demo_t;

static f64 now_seconds(void)
{
    return platform_time_to_seconds(platform_time_now());
}

// 帧描述里刻意带上尺寸与帧号：后端拿它算 texel 尺寸、shader 拿它做时间相关的抖动。
// 两个调用点（正常帧循环与 bench）共用这一个函数，免得一边改了另一边忘了。
static void fill_frame_desc(frame_desc_t *out, u32 width, u32 height, f64 delta_seconds,
                            u64 frame_index)
{
    memset(out, 0, sizeof(*out));
    out->target_width = width;
    out->target_height = height;
    out->delta_seconds = (f32)delta_seconds;
    out->elapsed_seconds = delta_seconds * (f64)(frame_index + 1u);
    out->frame_index = frame_index;
}

// LOGIC 阶段：把 Spin 累加进本地旋转。位置一个都不动 ——
// 轨道相机靠的就是"只转父节点，子节点的本地变换原样保留"。
static void spin_system(ecs_world_t *world, void *user, f32 delta_seconds)
{
    demo_t *demo = (demo_t *)user;
    if (demo == NULL || demo->spin_query == NULL) {
        return;
    }

    ecs_iter_t it;
    ecs_query_begin(demo->spin_query, &it);
    while (ecs_query_next(&it)) {
        ecs_transform_t  *t = ECS_COLUMN(&it, 0, ecs_transform_t);
        const demo_spin_t *spin = ECS_COLUMN(&it, 1, const demo_spin_t);
        if (t == NULL || spin == NULL || delta_seconds <= 0.0f) {
            continue;
        }
        const f32   radians = math_radians(spin->degrees_per_second * delta_seconds);
        quat_t      step;
        switch (spin->axis) {
        case 0u:
            step = quat_rotation_x(radians);
            break;
        case 2u:
            step = quat_rotation_z(radians);
            break;
        default:
            step = quat_rotation_y(radians);
            break;
        }
        // 左乘 = 绕世界轴转（右乘是绕自身轴）。轨道要的是绕场景的 Y，
        // 所以父节点转的时候子节点跟着画圆，而不是自己翻跟头。
        t->rotation = quat_normalize(quat_mul(step, t->rotation));
        demo->spins_applied++;
    }
    ecs_query_end(&it);
}

static bool demo_install_systems(demo_t *demo)
{
    ecs_world_t *world = scene_world(demo->scene);

    demo->spin = ecs_register_component(world, &k_spin_desc);
    if (demo->spin == ECS_COMPONENT_NONE) {
        LOG_ERROR("组件 Spin 注册失败");
        return false;
    }

    ecs_query_desc_t qd = ecs_query_desc_2(demo->ids.transform, demo->spin, "demo/spin");
    demo->spin_query = ecs_query_create(world, &qd);
    if (demo->spin_query == NULL) {
        LOG_ERROR("Spin 查询创建失败");
        return false;
    }

    const ecs_system_desc_t desc = { .name = "spin",
                                     .stage = ECS_STAGE_LOGIC,
                                     .priority = 0,
                                     .update = spin_system,
                                     .user = demo,
                                     .startup = NULL,
                                     .shutdown = NULL };
    if (ecs_add_system(world, &desc) == NULL) {
        LOG_ERROR("spin 系统注册失败");
        return false;
    }
    return true;
}

// =============================================================================
//  渲染资源：管线 + 程序化纹理
//
//  场景文件里只有**名字**（"pbr" / "checker" / "white"），句柄由这里给。
//  这就是三层结构的落点：存档→名字，运行期→表下标，GPU→句柄。
// =============================================================================
typedef struct assets_t {
    pipeline_handle_t pbr;
    pipeline_handle_t pbr_two_sided;
    pipeline_handle_t sprite;
    texture_handle_t  checker;
    texture_handle_t  white;
    mesh_handle_t     cube;
    bool              cube_procedural; // true = 存档里这条资源没有路径，读档后要重绑
} assets_t;

static bool assets_create(assets_t *a, renderer_t *renderer)
{
    memset(a, 0, sizeof(*a));
    a->pbr = PIPELINE_HANDLE_INVALID;
    a->pbr_two_sided = PIPELINE_HANDLE_INVALID;
    a->sprite = PIPELINE_HANDLE_INVALID;
    a->cube = MESH_HANDLE_INVALID;

    const shader_desc_t pbr_desc = shader_builtin_pbr_textured();
    const shader_desc_t solid_desc = shader_builtin_solid_color();
    const shader_handle_t shader_pbr = renderer_create_shader(renderer, &pbr_desc);
    const shader_handle_t shader_solid = renderer_create_shader(renderer, &solid_desc);
    if (!shader_is_valid(renderer, shader_pbr) || !shader_is_valid(renderer, shader_solid)) {
        LOG_ERROR("着色器创建失败");
        return false;
    }

    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_pbr;
        desc.raster.cull = CULL_MODE_BACK;
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "scene_pbr";
        a->pbr = renderer_create_pipeline(renderer, &desc);
    }
    {
        // 地面是单面片，相机绕到地平线以下会看到背面 —— 剔了就是"地面凭空消失"。
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_pbr;
        desc.raster.cull = CULL_MODE_NONE;
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "scene_pbr_two_sided";
        a->pbr_two_sided = renderer_create_pipeline(renderer, &desc);
    }
    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_solid;
        desc.raster.cull = CULL_MODE_NONE;
        desc.depth = depth_state_disabled();
        desc.blend = blend_state_alpha();
        desc.debug_name = "scene_sprite";
        a->sprite = renderer_create_pipeline(renderer, &desc);
    }
    if (!renderer_pipeline_is_valid(renderer, a->pbr) ||
        !renderer_pipeline_is_valid(renderer, a->pbr_two_sided) ||
        !renderer_pipeline_is_valid(renderer, a->sprite)) {
        LOG_ERROR("管线创建失败");
        return false;
    }

    const u8 checker_a[3] = { 232u, 232u, 236u };
    const u8 checker_b[3] = { 64u, 70u, 86u };
    a->checker = texture_create_checkerboard(renderer, 256u, 32u, checker_a, checker_b);
    a->white = texture_create_solid(renderer, 255u, 255u, 255u, 255u);
    if (!texture_is_valid(renderer, a->checker) || !texture_is_valid(renderer, a->white)) {
        LOG_ERROR("程序化纹理创建失败");
        return false;
    }
    return true;
}

// 名字 → 句柄。存档里**有路径**的资源由加载器自己建好，这里只补没有路径的那些，
// 所以必须在读档之后调用（未绑定的数量由 scene_pending_resource_count 报出来）。
static bool assets_bind_to_scene(scene_t *scene, const assets_t *a)
{
    if (!scene_register_pipeline(scene, "pbr", a->pbr) ||
        !scene_register_pipeline(scene, "pbr_two_sided", a->pbr_two_sided) ||
        !scene_register_pipeline(scene, "sprite", a->sprite)) {
        return false;
    }
    u32 id = SCENE_RESOURCE_NONE;
    if (!scene_add_atlas_handle(scene, "checker", a->checker, &id) ||
        !scene_add_atlas_handle(scene, "white", a->white, &id)) {
        return false;
    }
    if (a->cube_procedural && !scene_add_mesh_handle(scene, "cube", a->cube, &id)) {
        return false;
    }
    return true;
}

// =============================================================================
//  作者路径：造一份演示场景（只在没有存档或 --save 时跑）
// =============================================================================
static entity_t add_box(demo_t *demo, const char *name, vec3_t position, vec3_t scale, u32 mesh,
                        u32 material, bool cast_shadow, f32 spin_deg_per_s)
{
    ecs_world_t *world = scene_world(demo->scene);
    const entity_t e = ecs_create_entity(world);
    if (!ecs_transform_set_local(world, e, position, quat_identity(), scale)) {
        return ENTITY_NONE;
    }

    scene_name_t nm;
    memset(&nm, 0, sizeof(nm));
    snprintf(nm.text, sizeof(nm.text), "%s", name);
    if (ecs_add(world, e, demo->ids.name, &nm) == NULL) {
        return ENTITY_NONE;
    }

    scene_mesh_renderer_t mr;
    memset(&mr, 0, sizeof(mr));
    mr.mesh = mesh;
    mr.material = material;
    mr.layer = (u32)RENDER_LAYER_OPAQUE_3D;
    mr.cast_shadow = cast_shadow;
    if (ecs_add(world, e, demo->ids.mesh_renderer, &mr) == NULL) {
        return ENTITY_NONE;
    }

    if (spin_deg_per_s != 0.0f) {
        const demo_spin_t spin = { .degrees_per_second = spin_deg_per_s, .axis = 1u };
        if (ecs_add(world, e, demo->spin, &spin) == NULL) {
            return ENTITY_NONE;
        }
    }
    return e;
}

static void add_light(demo_t *demo, const char *name, u32 type, vec3_t position, vec3_t direction,
                      vec3_t color, f32 intensity, f32 range, bool cast_shadow)
{
    ecs_world_t *world = scene_world(demo->scene);
    const entity_t e = ecs_create_entity(world);
    (void)ecs_transform_set_local(world, e, position, quat_identity(), vec3_one());

    scene_name_t nm;
    memset(&nm, 0, sizeof(nm));
    snprintf(nm.text, sizeof(nm.text), "%s", name);
    (void)ecs_add(world, e, demo->ids.name, &nm);

    scene_light_t l;
    memset(&l, 0, sizeof(l));
    l.type = type;
    l.color = color;
    l.intensity = intensity;
    l.direction = direction; // **指向被照物体**（与 M4 的 light_t 同约定）
    l.range = range;
    l.inner_angle_degrees = 24.0f;
    l.outer_angle_degrees = 38.0f;
    l.cast_shadow = cast_shadow;
    (void)ecs_add(world, e, demo->ids.light, &l);
}

// 轨道相机：一个会转的空节点 + 挂在它下面的相机。
// 没有"相机轨道"代码 —— 层级本身就是轨道。
static void add_orbit_camera(demo_t *demo)
{
    ecs_world_t *world = scene_world(demo->scene);

    const entity_t rig = ecs_create_entity(world);
    (void)ecs_transform_set_local(world, rig, vec3_zero(), quat_identity(), vec3_one());
    scene_name_t rig_name;
    memset(&rig_name, 0, sizeof(rig_name));
    snprintf(rig_name.text, sizeof(rig_name.text), "%s", "camera_rig");
    (void)ecs_add(world, rig, demo->ids.name, &rig_name);
    const demo_spin_t rig_spin = { .degrees_per_second = 9.0f, .axis = 1u };
    (void)ecs_add(world, rig, demo->spin, &rig_spin);

    const entity_t cam = ecs_create_entity(world);
    // 相机在父节点的 -Z 远处，再绕 Y 转 180° 让它看向原点（约定：看向局部 -Z）。
    (void)ecs_transform_set_local(world, cam, vec3_make(0.0f, 4.5f, -13.0f), quat_rotation_y(F32_PI),
                                  vec3_one());
    (void)ecs_set_parent(world, cam, rig);

    scene_name_t cam_name;
    memset(&cam_name, 0, sizeof(cam_name));
    snprintf(cam_name.text, sizeof(cam_name.text), "%s", "main_camera");
    (void)ecs_add(world, cam, demo->ids.name, &cam_name);

    scene_camera_t c;
    memset(&c, 0, sizeof(c));
    c.projection = (u32)SCENE_CAMERA_PERSPECTIVE;
    c.fov_y_degrees = 55.0f;
    c.znear = 0.1f;
    c.zfar = 200.0f;
    c.active = true;
    (void)ecs_add(world, cam, demo->ids.camera, &c);
}

// out_prefab_root：作者路径顺手挑一个实体作为预制体的根，交给调用方导出。
static bool author_demo_scene(demo_t *demo, assets_t *assets, entity_t *out_prefab_root)
{
    scene_t     *scene = demo->scene;
    renderer_t  *renderer = scene_renderer(scene);
    ecs_world_t *world = scene_world(scene);

    u32 cube = SCENE_RESOURCE_NONE;
    if (!scene_add_mesh_from_file(scene, "cube", DEMO_MESH_PATH, &cube)) {
        LOG_WARN("%s 加载失败，改用程序化立方体", DEMO_MESH_PATH);
        assets->cube = mesh_create_cube(renderer, 1.0f);
        assets->cube_procedural = mesh_is_valid(renderer, assets->cube);
        if (!assets->cube_procedural || !scene_add_mesh_handle(scene, "cube", assets->cube, &cube)) {
            return false;
        }
    }

    u32 checker = SCENE_RESOURCE_NONE, white = SCENE_RESOURCE_NONE;
    if (!scene_add_atlas_handle(scene, "checker", assets->checker, &checker) ||
        !scene_add_atlas_handle(scene, "white", assets->white, &white)) {
        return false;
    }

    material_t metal = material_default();
    metal.tint = vec4_make(0.72f, 0.75f, 0.80f, 1.0f);
    material_set_pbr(&metal, 0.85f, 0.28f, 0.6f, 1.0f);
    u32 mat_metal = SCENE_RESOURCE_NONE;
    if (!scene_add_material(scene, "metal", "pbr", &metal, &mat_metal)) {
        return false;
    }

    material_t stone = material_default();
    stone.tint = vec4_make(0.85f, 0.82f, 0.78f, 1.0f);
    material_set_pbr(&stone, 0.0f, 0.78f, 0.3f, 1.0f);
    u32 mat_stone = SCENE_RESOURCE_NONE;
    if (!scene_add_material(scene, "stone", "pbr_two_sided", &stone, &mat_stone)) {
        return false;
    }
    // 槽位 0 = albedo（引擎还没定义具名槽位）。这一条同时是"材质→图集"那层
    // 依赖会被写进存档的证据：预制体实例化时图集跟着过来（test_prefab 钉着）。
    if (!scene_material_set_atlas(scene, mat_stone, 0u, checker)) {
        return false;
    }

    material_t glow = material_default();
    material_set_pbr(&glow, 0.1f, 0.4f, 0.4f, 1.0f);
    material_set_emissive(&glow, vec3_make(1.0f, 0.7f, 0.35f));
    u32 mat_glow = SCENE_RESOURCE_NONE;
    if (!scene_add_material(scene, "glow", "pbr", &glow, &mat_glow)) {
        return false;
    }

    // ---- 内容 ----
    (void)add_box(demo, "ground", vec3_make(0.0f, -0.30f, 0.0f), vec3_make(26.0f, 0.3f, 26.0f),
                  cube, mat_stone, false, 0.0f);

    const entity_t pillar_a =
        add_box(demo, "pillar_a", vec3_make(-4.5f, 1.6f, -1.5f), vec3_make(1.0f, 4.0f, 1.0f), cube,
                mat_stone, true, 0.0f);
    (void)add_box(demo, "pillar_b", vec3_make(4.5f, 1.6f, -1.5f), vec3_make(1.0f, 4.0f, 1.0f),
                  cube, mat_stone, true, 0.0f);
    (void)add_box(demo, "cube_spinning", vec3_make(0.0f, 2.4f, 0.0f), vec3_splat(1.6f), cube,
                  mat_metal, true, 42.0f);
    (void)add_box(demo, "cube_slow", vec3_make(2.2f, 0.9f, 3.2f), vec3_splat(0.9f), cube, mat_glow,
                  true, -14.0f);

    // 提着灯走：灯挂在一个会转的父节点下面，于是它绕着场景移动。
    // 组件里**没有**第二份坐标（存两份迟早不一致）。
    const entity_t arm =
        add_box(demo, "lamp_arm", vec3_zero(), vec3_one(), cube, mat_glow, false, 28.0f);
    if (entity_is_valid(arm)) {
        const entity_t lamp = ecs_create_entity(world);
        (void)ecs_transform_set_local(world, lamp, vec3_make(0.0f, 4.5f, 5.0f), quat_identity(),
                                      vec3_one());
        (void)ecs_set_parent(world, lamp, arm);
        add_light(demo, "lamp", 1u, vec3_make(0.0f, 4.5f, 5.0f), vec3_make(0.0f, -1.0f, 0.0f),
                  vec3_make(1.0f, 0.62f, 0.32f), 6.5f, 14.0f, false);
    }

    add_light(demo, "sun", 0u, vec3_zero(), vec3_make(-0.45f, -0.75f, 0.35f),
              vec3_make(1.0f, 0.96f, 0.90f), 3.1f, 0.0f, true);
    add_light(demo, "fill", 0u, vec3_zero(), vec3_make(0.6f, -0.35f, -0.6f),
              vec3_make(0.42f, 0.55f, 0.75f), 0.7f, 0.0f, false);

    add_orbit_camera(demo);

    // 一个 2D 精灵挂在 3D 场景里。M5 只要求它**能被提交**（计数会打印出来）；
    // 2D 在屏幕上的映射规则是 M6"2.5D 混合"要定义的东西。
    const entity_t sprite_e = ecs_create_entity(world);
    (void)ecs_transform_set_local(world, sprite_e, vec3_make(120.0f, 90.0f, 0.0f), quat_identity(),
                                  vec3_one());
    scene_name_t sprite_name;
    memset(&sprite_name, 0, sizeof(sprite_name));
    snprintf(sprite_name.text, sizeof(sprite_name.text), "%s", "hud_sprite");
    (void)ecs_add(world, sprite_e, demo->ids.name, &sprite_name);
    scene_sprite_renderer_t sr;
    memset(&sr, 0, sizeof(sr));
    sr.color = vec4_make(0.95f, 0.85f, 0.35f, 1.0f);
    sr.size = vec2_make(96.0f, 64.0f);
    sr.uv_min = vec2_make(0.0f, 0.0f);
    sr.uv_max = vec2_make(1.0f, 1.0f);
    sr.atlas = white;
    sr.layer = (u32)RENDER_LAYER_OPAQUE_2D;
    (void)ecs_add(world, sprite_e, demo->ids.sprite_renderer, &sr);

    // 环境：clear / 雾 / 天空盒 / 曝光 / 阴影开关与分辨率全从这里来。
    scene_env_t env = scene_env_default();
    env.clear_color = vec4_make(0.035f, 0.045f, 0.075f, 1.0f);
    env.ambient_color = vec3_make(0.52f, 0.58f, 0.72f);
    env.ambient_intensity = 0.24f;
    env.fog_enabled = true;
    env.fog_color = vec3_make(0.30f, 0.36f, 0.48f);
    env.fog_start = 16.0f;
    env.fog_end = 78.0f;
    env.skybox_enabled = true;
    env.exposure = 1.05f;
    env.shadows_enabled = true;
    env.shadow_strength = 0.82f;
    env.shadow_map_size = 2048u;
    env.culling_enabled = true;
    scene_set_environment(scene, &env);

    if (out_prefab_root != NULL) {
        *out_prefab_root = pillar_a;
    }
    return entity_is_valid(pillar_a);
}

// =============================================================================
//  装载
// =============================================================================
typedef struct load_report_t {
    bool  from_file;
    u32   entities;
    u32   components;
    u32   pending_before_bind;
    u32   pending_after_bind;
    u32   unknown_types;
    u32   dangling_refs;
    f64   load_ms;
    usize bytes;
} load_report_t;

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static void report_load(const load_report_t *r)
{
    printf("装载: %s | 实体 %u / 组件 %u | %.2f ms / %llu 字节\n",
           r->from_file ? "存档" : "现场生成", (unsigned)r->entities, (unsigned)r->components,
           (double)r->load_ms, (unsigned long long)r->bytes);
    printf("      资源绑定前未就位 %u 个，绑定后 %u 个；未知类型 %u，空引用 %u\n",
           (unsigned)r->pending_before_bind, (unsigned)r->pending_after_bind,
           (unsigned)r->unknown_types, (unsigned)r->dangling_refs);
}

// 优先读存档；读不到就现场 author 并写一份出去。
// force_author = true 时（--save）无条件重写，方便对着代码调数据。
static bool scene_load_or_author(demo_t *demo, assets_t *assets, const char *scene_path,
                                 bool force_author, entity_t *out_prefab_root,
                                 load_report_t *out_report)
{
    scene_t *scene = demo->scene;
    load_report_t r;
    memset(&r, 0, sizeof(r));

    if (!force_author && file_exists(scene_path)) {
        const f64 t0 = now_seconds();
        scene_io_stats_t stats;
        if (!scene_load_binary(scene, scene_path, &stats)) {
            LOG_ERROR("场景存档加载失败: %s", scene_path);
            return false;
        }
        r.load_ms = (now_seconds() - t0) * 1000.0;
        r.from_file = true;
        r.entities = stats.entities_loaded;
        r.components = stats.components_loaded;
        r.unknown_types = stats.unknown_component_types;
        r.dangling_refs = stats.dangling_refs;
        r.pending_before_bind = scene_pending_resource_count(scene);
        if (!assets_bind_to_scene(scene, assets)) {
            LOG_ERROR("读档后绑定资源失败");
            return false;
        }
        r.pending_after_bind = scene_pending_resource_count(scene);
        long size = -1;
        FILE *f = fopen(scene_path, "rb");
        if (f != NULL) {
            (void)fseek(f, 0, SEEK_END);
            size = ftell(f);
            fclose(f);
        }
        r.bytes = (size > 0) ? (usize)size : 0u;
        *out_report = r;
        report_load(&r);
        return true;
    }

    if (!author_demo_scene(demo, assets, out_prefab_root)) {
        LOG_ERROR("演示场景生成失败");
        return false;
    }
    scene_io_stats_t stats;
    if (!scene_save_binary(scene, scene_path, &stats)) {
        LOG_ERROR("场景存盘失败: %s（目录不存在？用 --scene 换个路径）", scene_path);
        return false;
    }
    r.entities = stats.entities_written;
    r.components = stats.components_written;
    r.bytes = 0u;
    *out_report = r;
    report_load(&r);
    return true;
}

// =============================================================================
//  预制体
// =============================================================================
// 在半径 radius 的圆上放一根柱子。index 决定角度，所以连按 N 会绕场一圈。
static bool spawn_pillar(prefab_t *prefab, scene_t *scene, u32 index, f64 *out_ms)
{
    const f64 t0 = now_seconds();
    entity_t root = ENTITY_NONE;
    if (!prefab_instantiate(prefab, scene, ENTITY_NONE, &root, NULL)) {
        return false;
    }
    const f32 angle = (f32)index * 0.55f;
    (void)ecs_transform_set_local(scene_world(scene), root,
                                  vec3_make(cosf(angle) * 7.5f, 1.6f, sinf(angle) * 7.5f),
                                  quat_rotation_y(-angle), vec3_one());
    if (out_ms != NULL) {
        *out_ms = (now_seconds() - t0) * 1000.0;
    }
    return true;
}

// =============================================================================
//  基准：N 个实体的 world_update 与提交的 CPU 耗时
//
//  刻意**不调 renderer_end_frame**：推队列是纯 CPU 的，而 end_frame 会把它们
//  真画出去 —— 10 万次 draw call 测的就不是 ECS 而是 GPU 了。
//  两个数分开打印，谁也别混进谁。
// =============================================================================
static void run_bench(demo_t *demo, assets_t *assets, renderer_t *renderer,
                      const allocator_tracking_t *tracker, u32 count)
{
    scene_t     *scene = demo->scene;
    ecs_world_t *world = scene_world(scene);

    if (count > DEMO_MAX_BENCH_ENTS) {
        LOG_ERROR("bench 上限是 %u 个实体（要更大就改 DEMO_MAX_BENCH_ENTS）",
                  (unsigned)DEMO_MAX_BENCH_ENTS);
        return;
    }

    u32 mesh = SCENE_RESOURCE_NONE, material = SCENE_RESOURCE_NONE;
    if (assets->cube_procedural) {
        (void)scene_add_mesh_handle(scene, "bench_cube", assets->cube, &mesh);
    } else if (!scene_add_mesh_from_file(scene, "bench_cube", DEMO_MESH_PATH, &mesh)) {
        assets->cube = mesh_create_cube(renderer, 1.0f);
        assets->cube_procedural = mesh_is_valid(renderer, assets->cube);
        (void)scene_add_mesh_handle(scene, "bench_cube", assets->cube, &mesh);
    }
    material_t m = material_default();
    material_set_pbr(&m, 0.3f, 0.5f, 0.4f, 1.0f);
    if (!scene_add_material(scene, "bench", "pbr", &m, &material)) {
        return;
    }

    const f64 build_t0 = now_seconds();
    const u32 side = (u32)sqrtf((f32)count);
    for (u32 i = 0u; i < count; ++i) {
        const entity_t e = ecs_create_entity(world);
        if (!entity_is_valid(e)) {
            LOG_ERROR("第 %u 个实体创建失败：世界容量不够（scene_create 的 entity_capacity）",
                      (unsigned)i);
            break;
        }
        const f32 x = (f32)(i % (side + 1u)) - (f32)side * 0.5f;
        const f32 z = (f32)(i / (side + 1u)) - (f32)side * 0.5f;
        (void)ecs_transform_set_local(world, e, vec3_make(x, 0.0f, z), quat_identity(),
                                      vec3_splat(0.55f));
        scene_mesh_renderer_t mr;
        memset(&mr, 0, sizeof(mr));
        mr.mesh = mesh;
        mr.material = material;
        mr.layer = (u32)RENDER_LAYER_OPAQUE_3D;
        mr.cast_shadow = (i % 3u) == 0u;
        (void)ecs_add(world, e, demo->ids.mesh_renderer, &mr);
        const demo_spin_t spin = { .degrees_per_second = (f32)(10u + (i % 80u)), .axis = 1u };
        (void)ecs_add(world, e, demo->spin, &spin);
    }
    const f64 build_ms = (now_seconds() - build_t0) * 1000.0;
    const u32 built = ecs_collect_entities(world, NULL, 0u);
    ecs_flush_commands(world);

    scene_submit_t sub;
    if (!scene_submit_create(&sub, scene, NULL)) { // 精灵批不需要：纯 3D 基准
        LOG_ERROR("提交桥接创建失败");
        return;
    }

    // 相机要**看着**那片网格阵列，否则"提交 193 / 剔除 193"这种数字会误导人：
    // 全被剔除了不是因为剔除算法好，而是因为相机朝反方向。
    camera_t camera = camera_default();
    camera.position = vec3_make(0.0f, (f32)sqrtf((f32)count) * 1.2f + 8.0f,
                                -(f32)sqrtf((f32)count) * 1.6f - 10.0f);
    camera.fov_y_radians = math_radians(55.0f);
    camera.aspect = 16.0f / 9.0f;
    camera.znear = 0.1f;
    camera.zfar = 4000.0f;
    camera_look_at(&camera, vec3_make(0.0f, 0.0f, 0.0f));
    // 刻意不调 renderer_end_frame：推队列是纯 CPU 的，画出去测的就不是 ECS 了。
    frame_desc_t frame;
    fill_frame_desc(&frame, 1280u, 720u, 1.0 / 60.0, 0u);

    // 三段时间**分开测**，因为"world_update 很慢"这个数没法定位问题：
    // ECS 的计算、渲染器的每帧状态、提交桥接是三个完全不同的东西。
    // 每段跑完立刻打印并 fflush —— 卡住的时候至少能看出卡在哪个循环里。
    f64 update_total = 0.0;
    for (u32 f = 0u; f < DEMO_BENCH_FRAMES; ++f) {
        const f64 t0 = now_seconds();
        (void)ecs_world_update(world, 1.0f / 60.0f);
        update_total += (now_seconds() - t0) * 1000.0;
    }
    printf("  [A] 纯 ecs_world_update      %8.3f ms/帧\n",
           (double)(update_total / DEMO_BENCH_FRAMES));
    fflush(stdout);

    // 先取一次统计再进 [B]：[B] 的第一次重建可能很慢，
    // 把这些数字先打出来才能看出"慢在深度、还是在节点数"。
    {
        ecs_world_stats_t probe;
        ecs_world_get_stats(world, &probe);
        printf("  [统计] 存活实体 %u，组件实例 %u，层级节点 %u，实测最大深度 %u，重建 %u 次\n",
               (unsigned)probe.entities_alive, (unsigned)probe.component_instances,
               (unsigned)ecs_hierarchy_node_count(world),
               (unsigned)probe.hierarchy_depth_max_seen, (unsigned)probe.hierarchy_rebuilds);
        fflush(stdout);
    }

    // 分阶段计时：层级重建 / 世界矩阵 / 系统本体。
    // 三个都是公开函数（公开的理由见各自头文件），所以这里能单独按成分测。
    f64 phase_rebuild = 0.0, phase_world = 0.0, phase_system = 0.0;
    for (u32 f = 0u; f < DEMO_BENCH_FRAMES; ++f) {
        const f64 t0 = now_seconds();
        (void)ecs_rebuild_hierarchy(world);
        const f64 t1 = now_seconds();
        ecs_update_world_transforms(world);
        const f64 t2 = now_seconds();
        spin_system(world, demo, 1.0f / 60.0f);
        const f64 t3 = now_seconds();
        phase_rebuild += (t1 - t0) * 1000.0;
        phase_world += (t2 - t1) * 1000.0;
        phase_system += (t3 - t2) * 1000.0;
    }

    // 提交桥接（含 begin_frame 的每帧状态），刻意不调 end_frame：
    // 画出去测的就不是 CPU 侧的提交成本了。
    f64 submit_total = 0.0;
    for (u32 f = 0u; f < DEMO_BENCH_FRAMES; ++f) {
        const f64 t0 = now_seconds();
        renderer_begin_frame(renderer, &camera, &frame);
        scene_submit_frame(&sub, renderer, 16.0f / 9.0f);
        submit_total += (now_seconds() - t0) * 1000.0;
    }
    const u32 submitted = sub.meshes_submitted;
    const u32 culled = renderer_culled_count(renderer);
    printf("  [C] begin_frame + submit   %8.3f ms/帧\n",
           (double)(submit_total / DEMO_BENCH_FRAMES));
    fflush(stdout);

    ecs_world_stats_t wstats;
    ecs_world_get_stats(world, &wstats);

    printf("\n== bench 汇总: %u 实体 ==\n", (unsigned)built);
    printf("  构建              %8.2f ms   (%.0f 实体/秒)\n", (double)build_ms,
           build_ms > 0.0 ? (double)built / (build_ms * 0.001) : 0.0);
    printf("  层级重建          %8.3f ms/帧\n", (double)(phase_rebuild / DEMO_BENCH_FRAMES));
    printf("  世界矩阵          %8.3f ms/帧\n", (double)(phase_world / DEMO_BENCH_FRAMES));
    printf("  spin 系统         %8.3f ms/帧\n", (double)(phase_system / DEMO_BENCH_FRAMES));
    printf("  提交              %8.3f ms/帧  (提交 %u，视锥剔除 %u)\n",
           (double)(submit_total / DEMO_BENCH_FRAMES), (unsigned)submitted, (unsigned)culled);
    printf("  组件实例 %u / 类型 %u / 重建次数 %u / 引擎内存在册 %llu KB\n",
           (unsigned)wstats.component_instances, (unsigned)wstats.component_types,
           (unsigned)wstats.hierarchy_rebuilds,
           (unsigned long long)(wstats.memory_bytes / 1024u));
    printf("  存活内存 %8.2f MiB   峰值 %8.2f MiB\n",
           (double)tracker->bytes_in_use / 1048576.0, (double)tracker->peak_bytes / 1048576.0);
    fflush(stdout);

    scene_submit_destroy(&sub);
}

// =============================================================================
//  命令行
// =============================================================================
typedef struct options_t {
    u32         width;
    u32         height;
    u32         frames;
    bool        hidden;
    bool        vsync;
    bool        save;
    u32         bench;
    u32         instances;
    const char *scene_path;
    const char *prefab_path;
} options_t;

static void print_usage(void)
{
    printf("用法: 06_scene [选项]\n");
    printf("  --scene PATH    场景存档（默认 assets/scenes/m5_demo.scene）\n");
    printf("  --prefab PATH   预制体（默认 assets/scenes/m5_pillar.prefab）\n");
    printf("  --save          重新生成存档与预制体后退出\n");
    printf("  --frames N      跑 N 帧后退出\n");
    printf("  --bench N       建 N 个实体测帧时间后退出（1k / 10k / 100k）\n");
    printf("  --instances N   启动时实例化 N 份预制体并计时\n");
    printf("  --hidden        不显示窗口\n");
    printf("  --no-vsync      关垂直同步\n");
    printf("\n运行时按键: ESC 退出 / N 加一根柱子 / P 存盘 / R 重新加载\n");
}

static bool parse_u32(const char *text, u32 *out)
{
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 0xFFFFFFFFul) {
        return false;
    }
    *out = (u32)value;
    return true;
}

// 需要跟一个数值的选项。返回 false = 参数缺失或非法。
static bool want_u32(int argc, char **argv, int *index, u32 *out, const char *flag)
{
    if (*index + 1 >= argc || !parse_u32(argv[*index + 1], out)) {
        printf("%s 需要一个非负整数参数\n", flag);
        return false;
    }
    (*index)++;
    return true;
}

static bool parse_options(int argc, char **argv, options_t *out)
{
    memset(out, 0, sizeof(*out));
    out->width = 1280u;
    out->height = 720u;
    out->vsync = true;
    out->scene_path = "assets/scenes/m5_demo.scene";
    out->prefab_path = "assets/scenes/m5_pillar.prefab";

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage();
            return false;
        } else if (strcmp(arg, "--width") == 0) {
            if (!want_u32(argc, argv, &i, &out->width, arg)) {
                return false;
            }
        } else if (strcmp(arg, "--height") == 0) {
            if (!want_u32(argc, argv, &i, &out->height, arg)) {
                return false;
            }
        } else if (strcmp(arg, "--frames") == 0) {
            if (!want_u32(argc, argv, &i, &out->frames, arg)) {
                return false;
            }
        } else if (strcmp(arg, "--bench") == 0) {
            if (!want_u32(argc, argv, &i, &out->bench, arg)) {
                return false;
            }
        } else if (strcmp(arg, "--instances") == 0) {
            if (!want_u32(argc, argv, &i, &out->instances, arg)) {
                return false;
            }
        } else if (strcmp(arg, "--scene") == 0 && i + 1 < argc) {
            out->scene_path = argv[++i];
        } else if (strcmp(arg, "--prefab") == 0 && i + 1 < argc) {
            out->prefab_path = argv[++i];
        } else if (strcmp(arg, "--hidden") == 0) {
            out->hidden = true;
        } else if (strcmp(arg, "--no-vsync") == 0) {
            out->vsync = false;
        } else if (strcmp(arg, "--save") == 0) {
            out->save = true;
        } else {
            printf("不认识参数: %s\n", arg);
            print_usage();
            return false;
        }
    }
    return true;
}

// =============================================================================
//  main
// =============================================================================
int main(int argc, char **argv)
{
    options_t options;
    if (!parse_options(argc, argv, &options)) {
        return 1;
    }

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    if (!log_init("06_scene.log", LOG_LEVEL_INFO)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }
    LOG_INFO("MyEngine %s —— M5 场景层", MYENGINE_VERSION_STRING);

    if (!platform_time_init(true)) {
        LOG_ERROR("计时系统初始化失败");
        log_shutdown();
        return 1;
    }

    int                exit_code = 1;
    platform_window_t *window = NULL;
    render_backend_t  *backend = NULL;
    renderer_t        *renderer = NULL;
    scene_t           *scene = NULL;
    sprite_batch_t     batch;
    shadow_map_t       shadow;
    bool               batch_ready = false;
    bool               shadow_ready = false;
    prefab_t          *prefab = NULL;
    scene_submit_t     sub;
    bool               sub_ready = false;
    demo_t             demo;
    assets_t           assets;
    u32                pillar_count = 0u;

    memset(&demo, 0, sizeof(demo));
    memset(&assets, 0, sizeof(assets));
    memset(&batch, 0, sizeof(batch));
    memset(&shadow, 0, sizeof(shadow));
    memset(&sub, 0, sizeof(sub));

    // ---- 窗口与渲染器 ----
    platform_window_desc_t window_desc = platform_window_desc_default();
    window_desc.title = "MyEngine M5 —— 场景层";
    window_desc.width = options.width;
    window_desc.height = options.height;
    window_desc.visible = !options.hidden;
    window_desc.resizable = true;

    if (!platform_window_create(&tracker.base, &window_desc, &window)) {
        LOG_ERROR("窗口创建失败");
        goto cleanup;
    }
    backend = gl_backend_create(&tracker.base);
    if (backend == NULL) {
        LOG_ERROR("GL 后端创建失败");
        goto cleanup;
    }
    render_backend_desc_t backend_desc;
    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.window = window;
    backend_desc.msaa_samples = 4;
    backend_desc.vsync = options.vsync;
    backend_desc.enable_debug_output = true;
    backend_desc.debug_name = "06_scene";
    if (!render_backend_init(backend, &backend_desc)) {
        LOG_ERROR("渲染后端初始化失败");
        goto cleanup;
    }
    renderer = renderer_create(&tracker.base, backend);
    if (renderer == NULL) {
        LOG_ERROR("渲染器创建失败");
        goto cleanup;
    }
    renderer_set_scene_target_enabled(renderer, true);

    sprite_batch_desc_t batch_desc = sprite_batch_desc_default();
    batch_desc.max_sprites = 4096u; // 精灵数超过它会自动分批，这里给的是"别在我这儿断批"
    if (!sprite_batch_init(&batch, renderer, &tracker.base, &batch_desc)) {
        LOG_ERROR("精灵批创建失败");
        goto cleanup;
    }
    batch_ready = true;

    if (!assets_create(&assets, renderer)) {
        LOG_ERROR("渲染资源创建失败");
        goto cleanup;
    }
    assets.cube_procedural = !file_exists(DEMO_MESH_PATH);
    if (assets.cube_procedural) {
        assets.cube = mesh_create_cube(renderer, 1.0f);
        assets.cube_procedural = mesh_is_valid(renderer, assets.cube);
        LOG_WARN("%s 读不到，改用程序化立方体（存档里这条资源将没有路径，读档后重新绑定）",
                 DEMO_MESH_PATH);
    }

    // ---- 场景 ----
    const u32 capacity = (options.bench > 0u)
                             ? ((options.bench > DEMO_MAX_BENCH_ENTS) ? DEMO_MAX_BENCH_ENTS
                                                                      : options.bench)
                             : 4096u;
    scene_config_t scene_config;
    memset(&scene_config, 0, sizeof(scene_config));
    scene_config.allocator = &tracker.base;
    scene_config.renderer = renderer;
    scene_config.name = "m5_demo";
    scene_config.entity_capacity = capacity;
    scene_config.resource_capacity_per_type = capacity;

    scene = scene_create(&scene_config);
    if (scene == NULL) {
        LOG_ERROR("场景创建失败");
        goto cleanup;
    }
    demo.scene = scene;
    if (!scene_register_components(scene, &demo.ids)) {
        LOG_ERROR("组件注册失败");
        goto cleanup;
    }
    // 管线的名字要在任何材质被解析之前登记好（解析发生在 scene_add_material 里），
    // 而句柄本身在读档之前就该就位 —— 存档里带路径的资源由加载器自己建，
    // 没路径的那些靠这里的名字覆盖。
    if (!assets_bind_to_scene(scene, &assets)) {
        LOG_ERROR("管线/图集登记失败");
        goto cleanup;
    }
    if (!demo_install_systems(&demo)) {
        LOG_ERROR("系统注册失败");
        goto cleanup;
    }

    // ---- 内容：读存档，或者现场生成一份 ----
    entity_t      prefab_root = ENTITY_NONE;
    load_report_t report;
    memset(&report, 0, sizeof(report));
    if (options.bench > 0u) {
        // bench 不读存档：它测的是 ECS 本身的每秒成本。
        // 混进存档的 13 个实体会让"世界容量 == count"变成"容量不够"，
        // 于是每次跑 bench 都先报一轮创建失败 —— 那是干扰项，不是被测对象。
        LOG_INFO("bench 模式：跳过存档加载，只测 ECS");
    } else if (!scene_load_or_author(&demo, &assets, options.scene_path, options.save,
                                     &prefab_root, &report)) {
        goto cleanup;
    }

    if (options.save) {
        if (!entity_is_valid(prefab_root) ||
            !scene_save_prefab(scene, prefab_root, options.prefab_path, NULL)) {
            LOG_ERROR("预制体导出失败");
            goto cleanup;
        }
        printf("已生成 %s 与 %s\n", options.scene_path, options.prefab_path);
        exit_code = 0;
        goto cleanup;
    }

    if (options.bench > 0u) {
        run_bench(&demo, &assets, renderer, &tracker, options.bench);
        exit_code = 0;
        goto cleanup;
    }

    prefab = prefab_load(&tracker.base, options.prefab_path);
    if (prefab == NULL) {
        LOG_WARN("预制体读不到（%s）：N 键不可用，其余照常", options.prefab_path);
    }
    if (options.instances > 0u) {
        const f64 t0 = now_seconds();
        u32 spawned = 0u;
        while (spawned < options.instances && spawn_pillar(prefab, scene, pillar_count, NULL)) {
            spawned++;
            pillar_count++;
        }
        const f64 ms = (now_seconds() - t0) * 1000.0;
        printf("预制体实例化 %u/%u 次，共 %.2f ms（%.3f ms/次），现在场景里有 %u 个实体\n",
               (unsigned)spawned, (unsigned)options.instances, (double)ms,
               spawned ? (double)(ms / spawned) : 0.0,
               (unsigned)ecs_collect_entities(scene_world(scene), NULL, 0u));
        if (spawned != options.instances) {
            LOG_ERROR("第 %u 次实例化失败（资源表或实体容量满了？）", (unsigned)(spawned + 1u));
        }
    }

    // ---- 提交桥接 + 阴影 ----
    if (!scene_submit_create(&sub, scene, &batch)) {
        LOG_ERROR("提交桥接创建失败");
        goto cleanup;
    }
    sub_ready = true;

    const scene_env_t *env = scene_environment(scene);
    if (env->shadows_enabled && env->shadow_map_size > 0u) {
        if (shadow_map_init(&shadow, renderer, env->shadow_map_size)) {
            shadow_ready = true;
            shadow_map_set_bias(&shadow, 2.5f, 1.8f, 0.035f);
            shadow_map_set_strength(&shadow, env->shadow_strength);
        } else {
            LOG_WARN("阴影贴图创建失败，将以无阴影模式继续");
        }
    }

    frame_clock_t   clock;
    frame_limiter_t limiter;
    frame_clock_init(&clock);
    frame_limiter_init(&limiter, 60.0);
    if (options.vsync) {
        limiter.enabled = false;
    }
    frame_clock_reset(&clock);

    char  title[256];
    f64   last_title = 0.0;
    bool  shadow_fitted = false;
    u32   frames = 0u;
    f64   reload_ms = 0.0;

    printf("\n开始渲染。ESC 退出 / N 加柱子 / P 存盘 / R 重新加载\n");

    while (true) {
        const platform_time_point_t frame_start = platform_time_now();

        if (!platform_window_poll_events(window)) {
            break;
        }
        const input_state_t *input = platform_window_input(window);
        if (input_key_pressed(input, KEY_ESCAPE)) {
            break;
        }
        if (input_key_pressed(input, KEY_N) && prefab != NULL) {
            if (pillar_count < DEMO_PILLAR_LIMIT) {
                if (!spawn_pillar(prefab, scene, pillar_count++, NULL)) {
                    LOG_ERROR("实例化失败（资源表或实体容量满了？）");
                }
            } else {
                LOG_WARN("柱子数量到上限 %u 了（DEMO_PILLAR_LIMIT）", DEMO_PILLAR_LIMIT);
            }
        }
        if (input_key_pressed(input, KEY_P)) {
            scene_io_stats_t stats;
            if (scene_save_binary(scene, options.scene_path, &stats)) {
                printf("已存盘：%u 个实体 / %u 个组件\n", (unsigned)stats.entities_written,
                       (unsigned)stats.components_written);
            }
        }
        if (input_key_pressed(input, KEY_R)) {
            // 加载**不**清空世界（隐式副作用在调用点上看不出来），
            // 所以"重新加载"就是显式的两行：先清，再读。
            const f64 t0 = now_seconds();
            ecs_world_clear(scene_world(scene));
            scene_io_stats_t stats;
            if (scene_load_binary(scene, options.scene_path, &stats)) {
                (void)assets_bind_to_scene(scene, &assets);
                reload_ms = (now_seconds() - t0) * 1000.0;
                shadow_fitted = false;
                pillar_count = 0u;
                printf("重新加载：%u 个实体 / %.2f ms\n", (unsigned)stats.entities_loaded,
                       (double)reload_ms);
            } else {
                LOG_ERROR("重新加载失败，画面保持上一帧的世界");
            }
        }

        const f64 delta = frame_clock_tick(&clock);
        demo.last_spins = demo.spins_applied;
        demo.spins_applied = 0u;

        // ---- 顺序：先跑世界，再 begin_frame，最后提交 ----
        //
        // 为什么 scene_submit_frame 不注册成 RENDER_SUBMIT 阶段的系统：
        // renderer_begin_frame 需要一个相机，而相机要从**本帧已经算完的**世界矩阵里取。
        // 提交若是系统，就必须先 begin_frame 再 update，那个相机只能是上一帧的。
        // 一帧的延迟在 60fps 下看不出来，但"剔除和阴影用的是哪一帧的相机"
        // 值得用一条注释解决，而不是留给下一个人猜。
        (void)ecs_world_update(scene_world(scene), (f32)delta);

        u32 width = options.width, height = options.height;
        platform_window_get_size(window, &width, &height);
        const f32 aspect = (height > 0u) ? (f32)width / (f32)height : 1.0f;

        camera_t camera = camera_default();
        const bool has_camera = scene_submit_active_camera(&sub, aspect, &camera);
        frame_desc_t frame;
        fill_frame_desc(&frame, width, height, delta, (u64)frames);
        renderer_begin_frame(renderer, &camera, &frame);

        sprite_batch_begin(&batch);
        scene_submit_frame(&sub, renderer, aspect);
        sprite_batch_end(&batch);

        if (shadow_ready) {
            if (!shadow_fitted && has_camera) {
                // 主光的朝向来自场景（存档里的那盏平行光）。
                // 第一帧提交之后 renderer 里就有了它，这时候拟合一次阴影视锥。
                const light_t *key = light_set_get(renderer_lights(renderer), 0u);
                if (key != NULL && key->cast_shadow) {
                    shadow_map_fit_directional(&shadow, key, vec3_make(0.0f, 1.5f, 0.0f), 22.0f);
                    shadow_fitted = true;
                }
            }
            if (shadow_fitted) {
                shadow_map_render(&shadow, renderer);
            }
        }
        renderer_end_frame(renderer);

        frames++;
        if (options.frames > 0u && frames >= options.frames) {
            exit_code = 0;
            break;
        }

        if (clock.elapsed_seconds - last_title > 0.25) {
            last_title = clock.elapsed_seconds;
            render_stats_t stats;
            renderer_get_stats(renderer, &stats);
            snprintf(title, sizeof(title),
                     "MyEngine M5 | %.0f fps | 实体 %u | 提交 %d 精灵 %d 剔除 %u | Spin %u | "
                     "柱子 %u",
                     (double)clock.fps,
                     (unsigned)ecs_collect_entities(scene_world(scene), NULL, 0u),
                     (int)sub.meshes_submitted, (int)sub.sprites_submitted,
                     (unsigned)renderer_culled_count(renderer), (unsigned)demo.last_spins,
                     (unsigned)pillar_count);
            platform_window_set_title(window, title);
        }

        frame_limiter_wait(&limiter, frame_start);
    }

    // 打在 cleanup 之前：这个数要包含场景与预制体自己的占用，
    // 拆完之后再问峰值就没意义了。
    printf("\n退出：%u 帧 | 场景来自 %s | 峰值内存 %llu KB\n", (unsigned)frames,
           report.from_file ? "存档文件" : "代码现场生成",
           (unsigned long long)(tracker.peak_bytes / 1024u));

cleanup:
    if (shadow_ready) {
        shadow_map_shutdown(&shadow, renderer);
    }
    if (sub_ready) {
        scene_submit_destroy(&sub);
    }
    if (prefab != NULL) {
        prefab_destroy(prefab);
    }
    if (batch_ready) {
        sprite_batch_shutdown(&batch);
    }
    // 查询是**世界分配**的，必须显式销毁，而且要在场景（也就是世界）之前。
    // 少这一行的症状很隐蔽：程序正常退出、画面正常，只有泄漏检查会报 96 字节。
    if (demo.spin_query != NULL) {
        ecs_query_destroy(scene_world(scene), demo.spin_query);
        demo.spin_query = NULL;
    }
    if (scene != NULL) {
        scene_destroy(scene);
        scene = NULL;
    }
    if (renderer != NULL) {
        renderer_destroy(renderer);
    }
    if (backend != NULL) {
        // gl_backend_destroy 内部会 shutdown（拆 GL 上下文与它自己的状态），
        // 所以这里不再单独调 render_backend_shutdown —— 两次释放同一批 GPU 对象
        // 是那种"Release 版偶发崩"的源头。
        gl_backend_destroy(backend);
    }
    if (window != NULL) {
        platform_window_destroy(window);
    }
    platform_time_shutdown();

    // 这个示例跑的是完整生命周期，所以泄漏检查是真的在检查：
    // 场景、预制体、精灵批、阴影各自的分配都要成对收回。
    // 两个分支都要说话：只报坏消息的验收清单，"没消息"会被当成"没检查"。
    if (allocator_tracking_has_leak(&tracker)) {
        fprintf(stderr, "检测到内存泄漏：%llu 块 / %llu 字节未释放\n",
                (unsigned long long)tracker.live_blocks,
                (unsigned long long)tracker.bytes_in_use);
        exit_code = 1;
    } else {
        printf("内存泄漏             : 无（%llu 字节全部归还）\n",
               (unsigned long long)tracker.total_bytes);
    }
    log_shutdown();
    return exit_code;
}
