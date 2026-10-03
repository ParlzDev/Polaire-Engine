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
//  author_demo_scene() 只在"存档不存在"或 --save 时跑，它存在的唯一理由是把那两份
//  存档造出来（m5_demo.scene 与 m5_demo.scene.json —— 同一次导出，两种编码）。
//  之后 .scene.json 就是可以手改的那一份：字段名与组件名都是明文，
//  没写的字段取默认值。M11 的编辑器会取代 author_demo_scene()。
//
//  验证 M5 的哪几条：
//    * 实体 + 组件：网格/精灵/相机/灯/名字 + 一个项目级组件 Spin
//    * 固定阶段调度：Spin 在 LOGIC 阶段，世界矩阵在 TRANSFORM 阶段，同一帧内成立
//    * 父子层级：3 层链（地面→柱身→柱顶灯臂），动父节点子节点跟着走
//    * 场景可存读：.scene 与 .scene.json **两种编码描述同一份数据**，
//      每次启动都当场自检（二进制 ↔ JSON 转码逐字节相同，失败就退出码非 0）；
//      更细的判据由 tests/test_scene_io.c 钉住
//    * 预制体：.prefab 可反复实例化，资源依赖跟着走
//    * 2D 与 3D 同帧共存：靠 render layer 区分，不靠两套架构
//    * ECS→渲染桥接：画面里的一切都经 scene_submit_frame
//    * 1k/10k/100k 实体的帧时间与内存：--bench N
//
//  操作：
//      ESC   退出
//      N     加一根柱子（实例化一次预制体）
//      P     把当前场景存回 .scene 与 .scene.json
//      R     清空世界，重新从存档加载
//      1/2   在二进制与 JSON 两份存档之间切换重载（证明两者渲染结果一致）
//
//  命令行：
//      --scene PATH      场景存档路径（默认 assets/scenes/m5_demo.scene；
//                        给 .json 后缀就读 JSON 那份，实际按内容判定）
//      --prefab PATH     预制体路径（默认 assets/scenes/m5_pillar.prefab）
//      --save            重新生成存档（两种编码）与预制体后退出
//      --check-pixel     读回若干像素验证 2D/3D 同帧与色彩管线，失败则退出码非 0
//      --frames N        跑 N 帧后退出（CI 用）
//      --bench N         建 N 个实体，测 world_update / 提交 / 整帧的 CPU 耗时后退出
//      --instances N     启动后立刻实例化 N 份预制体并计时
//      --hidden / --no-vsync / --width / --height
// =============================================================================
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"

// ecs/ 与 scene/ 刻意不在 engine.h 里（理由见那份注释），示例自己按需包含。
#include <errno.h>
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

#ifdef _WIN32
#include <direct.h>
// MSVC 的 CRT 给 POSIX 名字加了警告（"用 ISO 名字"），所以 Windows 上走 _mkdir/_chdir。
#define EXAMPLE_MKDIR(path) _mkdir(path)
#define EXAMPLE_CHDIR(path) _chdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define EXAMPLE_MKDIR(path) mkdir((path), 0777)
#define EXAMPLE_CHDIR(path) chdir(path)
#endif

#define DEMO_MAX_BENCH_ENTS 200000u
#define DEMO_BENCH_FRAMES 30u
#define DEMO_PILLAR_LIMIT 64u
#define DEMO_MESH_PATH "assets/models/cube.mesh"

// ---------------------------------------------------------------- 工作目录

// 从当前目录往上找资源根（有 assets/ 的那一层），找到就把工作目录切过去。
//
// 为什么切目录而不是把每条路径各自拼成绝对路径：示例与**存档文件里记的都是相对路径**
// （"assets/models/cube.mesh"），存档里的路径必须跟着资产走，绝对路径反而没法搬。
// 切一次 cwd 之后读、写、以及存档内部记录的路径全都一致。
// 从 build/... 里直接双击 exe 是这个项目最常见的"资产读不到 + 存盘失败"来源，
// 而症状看起来像引擎坏了。
//
// 判据用"能不能建 assets/scenes"而不是 fopen 某个文件：一次解决两件事 ——
// 认出资源根（父目录不存在时 mkdir 会失败），并且备好示例要写 .scene 的那个目录
// （目录不存在时 fopen("wb") 直接失败，报错却只说"无法写入"）。
static bool settle_assets_root(u32 *out_levels_up)
{
    for (u32 up = 0u; up <= 6u; ++up) {
        if (EXAMPLE_MKDIR("assets/scenes") == 0 || errno == EEXIST) {
            *out_levels_up = up;
            return true;
        }
        if (up == 6u) {
            break;
        }
        if (EXAMPLE_CHDIR("..") != 0) {
            return false; // 到顶了
        }
    }
    return false;
}

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

// 挂在某个父节点下面的盒子。和 add_box 分开写，是因为父子链上每一层要的组件不同：
// 根那一层带 Spin（会自己转），中间层只是挂点，叶子带灯。
// 硬塞成一个函数加一堆可选参数，读的人反而看不出哪层挂了些什么。
static entity_t add_child_box(demo_t *demo, const char *name, entity_t parent, vec3_t position,
                              vec3_t scale, u32 mesh, u32 material, bool cast_shadow)
{
    ecs_world_t *world = scene_world(demo->scene);
    const entity_t e = add_box(demo, name, position, scale, mesh, material, cast_shadow, 0.0f);
    if (!entity_is_valid(e)) {
        return ENTITY_NONE;
    }
    if (!entity_is_valid(parent) || !ecs_set_parent(world, e, parent)) {
        // 挂不上就是 D4 的两道防线之一在起作用（环或超深当场拒绝）
        LOG_ERROR("%s 挂不到父节点上（父子环？超深？）", name);
        return ENTITY_NONE;
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
    ecs_world_t *world = scene_world(scene);

    // 网格从哪儿来由 main 一次决定（assets_create 之后那次 file_exists 检查）。
    // 这里以前自己也去读一次文件，于是同一个缺失资产会报两遍错 ——
    // 两个地方各自判断"能不能读到"，就必然出现"一处以为能读到、另一处读到失败"。
    u32 cube = SCENE_RESOURCE_NONE;
    if (assets->cube_procedural) {
        if (!scene_add_mesh_handle(scene, "cube", assets->cube, &cube)) {
            return false;
        }
    } else if (!scene_add_mesh_from_file(scene, "cube", DEMO_MESH_PATH, &cube)) {
        LOG_ERROR("%s 存在但读不出来（文件坏了？）", DEMO_MESH_PATH);
        return false;
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

    // 提着灯走：灯挂在一条**四层**链的最深处（rig → arm → head → light）。
    // 组件里没有第二份坐标（存两份迟早不一致），而链长是 D4 那条
    // "拍平拓扑序"的判据本身 —— 两层链在一帧内看不出求值顺序错没错。
    const entity_t rig = add_box(demo, "lamp_rig", vec3_zero(), vec3_one(), cube, mat_glow, false,
                                 28.0f);
    entity_t chain_leaf = ENTITY_NONE;
    if (entity_is_valid(rig)) {
        const entity_t arm = add_child_box(demo, "lamp_arm", rig, vec3_make(0.0f, 3.2f, 0.0f),
                                           vec3_one(), cube, mat_glow, false);
        const entity_t head = add_child_box(demo, "lamp_head", arm, vec3_make(0.0f, 1.1f, 1.6f),
                                            vec3_splat(0.55f), cube, mat_glow, true);
        chain_leaf = ecs_create_entity(world);
        (void)ecs_transform_set_local(world, chain_leaf, vec3_make(0.0f, 0.45f, 0.0f),
                                      quat_identity(), vec3_one());
        (void)ecs_set_parent(world, chain_leaf, head);
        scene_name_t leaf_name;
        memset(&leaf_name, 0, sizeof(leaf_name));
        snprintf(leaf_name.text, sizeof(leaf_name.text), "%s", "lamp_light");
        (void)ecs_add(world, chain_leaf, demo->ids.name, &leaf_name);
        scene_light_t lt;
        memset(&lt, 0, sizeof(lt));
        lt.type = 1u; // 点光
        lt.color = vec3_make(1.0f, 0.62f, 0.32f);
        lt.intensity = 6.5f;
        lt.range = 14.0f;
        lt.direction = vec3_make(0.0f, -1.0f, 0.0f);
        (void)ecs_add(world, chain_leaf, demo->ids.light, &lt);
        // 链上的每一层都要有世界变换，否则"叶子跟着根走"这件事没有承载体
        (void)ecs_transform_get_or_add(world, chain_leaf);
    }

    add_light(demo, "sun", 0u, vec3_zero(), vec3_make(-0.45f, -0.75f, 0.35f),
              vec3_make(1.0f, 0.96f, 0.90f), 3.1f, 0.0f, true);
    add_light(demo, "fill", 0u, vec3_zero(), vec3_make(0.6f, -0.35f, -0.6f),
              vec3_make(0.42f, 0.55f, 0.75f), 0.7f, 0.0f, false);

    add_orbit_camera(demo);

    // 一个 2D 精灵和 3D 物体在同一个世界、同一个队列里。
    // 位置用的是**世界单位**（精灵的顶点本来就是世界空间，z 被 sprite 着色器固定为 0），
    // 所以它现在是一张贴在场景里的图；"HUD 那种屏幕像素映射"是 M6 的 2.5D 混合要定的规则。
    // 放在这里是为了能被 --check-pixel 读回：摘掉它画面必须变，才算 2D 真的进了这一帧。
    const entity_t sprite_e = ecs_create_entity(world);
    (void)ecs_transform_set_local(world, sprite_e, vec3_make(-6.0f, 3.0f, 0.0f), quat_identity(),
                                  vec3_one());
    scene_name_t sprite_name;
    memset(&sprite_name, 0, sizeof(sprite_name));
    snprintf(sprite_name.text, sizeof(sprite_name.text), "%s", "hud_sprite");
    (void)ecs_add(world, sprite_e, demo->ids.name, &sprite_name);
    scene_sprite_renderer_t sr;
    memset(&sr, 0, sizeof(sr));
    sr.color = vec4_make(0.95f, 0.85f, 0.35f, 1.0f);
    sr.size = vec2_make(2.6f, 1.8f); // 世界单位：这张"贴图"高 1.8 米
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

// 由一个存档路径推出**另一种编码**的路径：.scene ↔ .scene.json。
// 为什么用"同目录同前缀 + 换后缀"而不是加个 --scene-json 参数：
// 两种编码描述的是同一份数据，路径差一个后缀就能让人把它们当成两个资产看待,
// 而"改了 .scene.json 但 .scene 还是旧的"这种状态是最容易让人误判的。
static void sibling_format_path(const char *path, char *out, usize cap)
{
    const usize n = strlen(path);
    if (n > 5u && strcmp(path + n - 5u, ".json") == 0) {
        snprintf(out, cap, "%.*s", (int)(n - 5u), path); // 去掉 .json → 二进制那份
        return;
    }
    snprintf(out, cap, "%s.json", path);
}

// 两种编码的等价性自检：当场做一遍，而不是只信单元测试。
//
//   场景 --写--> 二进制 X，--写--> JSON J，J --转码--> 二进制 X2
//   要求 X 与 X2 逐字节相同。
//
// 为什么值得在示例里再做一遍：单元测试用的是离线场景（没有 renderer、没有真实
// 资源路径），而这里这份存档是**要进仓库**的资产。两者输入不同，判据相同。
// 失败一定要退出码非 0：自检只在"跑一遍就没人看"的情况下才有意义的话，
// 它就成了装饰。
static bool verify_format_equivalence(scene_t *scene)
{
    arena_t *scratch = scene_scratch_arena(scene);
    const arena_mark_t mark = arena_mark(scratch);

    void *binary = NULL;
    usize binary_size = 0u;
    void *text = NULL;
    usize text_length = 0u;
    bool ok = scene_write_binary(scene, scratch, &binary, &binary_size, NULL) &&
              scene_write_json(scene, scratch, &text, &text_length, NULL);
    usize back_size = 0u;
    void *back = NULL;
    if (ok) {
        ok = scene_binary_from_json(scene, (const char *)text, text_length, scratch, &back,
                                    &back_size);
    }
    if (ok && (back_size != binary_size || memcmp(binary, back, binary_size) != 0)) {
        usize diff = 0u;
        while (diff < binary_size && diff < back_size &&
               ((const u8 *)binary)[diff] == ((const u8 *)back)[diff]) {
            diff++;
        }
        LOG_ERROR("两种编码不等价：二进制 %llu 字节，JSON 转回 %llu 字节，首个差异在 %llu",
                  (unsigned long long)binary_size, (unsigned long long)back_size,
                  (unsigned long long)diff);
        ok = false;
    }
    if (ok) {
        LOG_INFO("格式自检：二进制(%llu B) ↔ JSON(%llu B) 逐字节一致",
                 (unsigned long long)binary_size, (unsigned long long)text_length);
    }
    arena_rewind(scratch, mark);
    return ok;
}

// 两种编码各存一份。第二个路径由第一个推出来。
// stats 只从**二进制**那一次取（同一份数据写两遍，计数当然相同；
// 取两次再判等就变成给自检加第二套逻辑了）。
static bool save_both_formats(scene_t *scene, const char *path, scene_io_stats_t *stats)
{
    char sibling[512];
    sibling_format_path(path, sibling, sizeof(sibling));
    const bool json_first = scene_path_is_json(path);
    return json_first ? (scene_save_json(scene, path, stats) && scene_save_binary(scene, sibling, NULL))
                      : (scene_save_binary(scene, path, stats) && scene_save_json(scene, sibling, NULL));
}

// 清世界 → 读档 → 绑资源 → 格式自检。
//
// 加载**不**清空世界（隐式副作用在调用点上看不出来），所以"重新加载"
// 就是显式的两行：先清，再读。这个函数只是把那两行连同绑资源与自检收在一起 ——
// 示例里有两个触发点（R 原样重载、1/2 换编码），抄两遍迟早有一遍忘了绑资源。
static bool reload_from(scene_t *scene, assets_t *assets, const char *path, f64 *out_ms,
                        u32 *out_entities)
{
    const f64 t0 = now_seconds();
    ecs_world_clear(scene_world(scene));
    scene_io_stats_t stats;
    if (!scene_load_auto(scene, path, &stats)) {
        return false;
    }
    if (!assets_bind_to_scene(scene, assets)) {
        LOG_ERROR("读档后绑定资源失败");
        return false;
    }
    if (!verify_format_equivalence(scene)) {
        return false;
    }
    *out_ms = (now_seconds() - t0) * 1000.0;
    *out_entities = stats.entities_loaded;
    return true;
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
        if (!scene_load_auto(scene, scene_path, &stats)) {
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
    if (!save_both_formats(scene, scene_path, &stats)) {
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
                      const allocator_tracking_t *tracker, u32 count, bool vsync)
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

    // ---- 精灵也要有：M3/M4 的"含渲染 60 FPS"判据问的是**混排**的那一帧 ----
    //
    // 只测方块的话，2D 的合批、贴图切换、transparent layer 全都不在这条时间里；
    // 而"M3 的 1000 实体 60 FPS（含渲染）在 M4 末正式验证"这条判据
    // 的原话是"同一帧里 1 万静态方块 + 1000 动图 sprites"。
    // 数量取实体的 1/10：10k 方块正好配 1000 精灵，规模变了比例不变。
    u32 sprites_built = 0u;
    u32 white = SCENE_RESOURCE_NONE;
    u32 sprite_want = count / 10u;
    if (sprite_want > 0u) {
        if (!scene_add_atlas_handle(scene, "bench_white", assets->white, &white)) {
            LOG_ERROR("bench 图集登记失败，跳不过精灵那一半的压测");
            sprite_want = 0u;
        }
    }
    for (u32 i = 0u; sprite_want > 0u && i < sprite_want; ++i) {
        const entity_t e = ecs_create_entity(world);
        if (!entity_is_valid(e)) {
            LOG_ERROR("第 %u 个精灵实体创建失败：世界容量不够", (unsigned)i);
            break;
        }
        // 摆在相机与方块阵列之间的一条带上；每张给一个不整数化的尺寸，
        // 免得合批后的顶点坐标因为"都落在像素网格上"而比真实情况便宜
        const f32 x = -22.0f + (f32)(i % 50u) * 0.9f;
        const f32 y = 1.2f + (f32)(i / 50u) * 0.9f;
        (void)ecs_transform_set_local(world, e, vec3_make(x, y, 3.0f), quat_identity(),
                                      vec3_one());
        scene_sprite_renderer_t sr;
        memset(&sr, 0, sizeof(sr));
        sr.color = vec4_make(0.9f, 0.55f, 0.2f, 1.0f);
        sr.size = vec2_make(0.7f + (f32)(i % 7u) * 0.05f, 0.55f + (f32)(i % 5u) * 0.04f);
        sr.uv_min = vec2_make(0.0f, 0.0f);
        sr.uv_max = vec2_make(1.0f, 1.0f);
        sr.atlas = white;
        sr.layer = (u32)RENDER_LAYER_TRANSPARENT_2D;
        if (ecs_add(world, e, demo->ids.sprite_renderer, &sr) == NULL) {
            break;
        }
        // "动图"：会转的精灵。旋转走的是同一份 Transform，所以这里复用 Spin，
        // 而不是给精灵再造一套动画状态（两套状态迟早和拓扑序对不上）。
        const demo_spin_t spin = { .degrees_per_second = 40.0f + (f32)(i % 9u), .axis = 2u };
        if (ecs_add(world, e, demo->spin, &spin) == NULL) {
            break;
        }
        sprites_built++;
    }

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

    // [D] **整帧**：把 end_frame（present / 交换缓冲）也算进来。
    //
    // 为什么 [C] 不够：[C] 是 CPU 侧录制命令的成本，而"1000 实体 60 FPS"这条
    // 判据问的是能不能真的把画面送出去。GPU 提交与交换不在 [C] 里，
    // 报 [C] 当帧率会把一个纯 CPU 数字当成端到端数字用。
    //
    // 读这一行的时候注意两件事：
    //   1. vsync 开着时这个数字会被显示器的 16.7 ms 钳住 —— 那说明"够 60 FPS"，
    //      但看不出还剩多少头程；要头程就用 --no-vsync 再跑一遍。
    //   2. 它是**墙钟**时间，所以包含了驱动在命令队列满时的等待 ——
    //      这正是"GPU 跟不上"会以什么形式出现：不是错误，是这一行变大。
    f64 full_total = 0.0;
    f64 full_worst = 0.0;
    f64 full_best = 1e30;
    for (u32 f = 0u; f < DEMO_BENCH_FRAMES; ++f) {
        const f64 t0 = now_seconds();
        (void)ecs_world_update(world, 1.0f / 60.0f);
        renderer_begin_frame(renderer, &camera, &frame);
        scene_submit_frame(&sub, renderer, 16.0f / 9.0f);
        renderer_end_frame(renderer);
        const f64 ms = (now_seconds() - t0) * 1000.0;
        full_total += ms;
        if (ms > full_worst) {
            full_worst = ms;
        }
        if (ms < full_best) {
            full_best = ms;
        }
    }
    const f64 full_ms = full_total / DEMO_BENCH_FRAMES;
    // 最快/平均/最差一起印，而不是只印平均。
    //
    // 理由不是排版偏好：这个数含 present，于是**驱动什么时候节流、别的进程有没有
    // 在用 GPU**都会进来到墙钟里。同一台机器同一份二进制，实测在 1.6 与 10.4 ms
    // 之间跳过 —— 只印一个平均数的话，下一个人抄进文档的就是一个看似精确、
    // 实则无法复现的标量。给出区间，读者自己就能判断"够不够 60 FPS"这句话的硬度。
    printf("  [D] 混排整帧（方块 %u + 精灵 %u，含 end_frame/present 墙钟） 平均 %8.3f ms = %.1f FPS"
           "，最快 %.3f / 最差 %.3f ms%s\n",
           (unsigned)built, (unsigned)sprites_built, (double)full_ms,
           full_ms > 0.0 ? 1000.0 / (double)full_ms : 0.0, (double)full_best,
           (double)full_worst, vsync ? "（vsync 开着，会被显示器钳住）" : "");
    if (sprite_want > 0u && sprites_built != sprite_want) {
        // 压测的**内容**没建起来，报出来的帧率就是在测别的东西。
        LOG_ERROR("精灵只建出 %u/%u 个：这条 [D] 不是混排数字（容量不够？）",
                  (unsigned)sprites_built, (unsigned)sprite_want);
    }
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
    // 路径是**可写的**：按 1/2 在两种编码之间切换时要换掉它。
    // 为什么不用 const char*：那会让"当前用的是哪份存档"这件事存在两处
    // （指针本身 + 一个"现在是 JSON 吗"的布尔），而两处迟早会各说一套。
    char        scene_path[512];
    char        prefab_path[512];
    bool        check_pixel;
} options_t;

// 装路径。超长直接拒绝而不是截断：截断后的路径指向**另一个可能存在的文件**，
// 那比"启动失败"难排查得多。
static bool set_path(char *dst, usize cap, const char *src)
{
    if (src == NULL || strlen(src) + 1u > cap) {
        LOG_ERROR("路径过长（上限 %u 字节）", (unsigned)cap);
        return false;
    }
    snprintf(dst, cap, "%s", src);
    return true;
}

static void print_usage(void)
{
    printf("用法: 06_scene [选项]\n");
    printf("  --scene PATH    场景存档（默认 assets/scenes/m5_demo.scene）\n");
    printf("  --prefab PATH   预制体（默认 assets/scenes/m5_pillar.prefab）\n");
    printf("  --save          重新生成存档与预制体后退出\n");
    printf("  --frames N      跑 N 帧后退出\n");
    printf("  --bench N       建 N 个实体测帧时间后退出（1k / 10k / 100k）\n");
    printf("  --instances N   启动时实例化 N 份预制体并计时\n");
    printf("  --check-pixel   读回画面像素，验证 2D/3D 同帧与 sRGB 管线；不符则退出码非 0\n");
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
    snprintf(out->scene_path, sizeof(out->scene_path), "%s", "assets/scenes/m5_demo.scene");
    snprintf(out->prefab_path, sizeof(out->prefab_path), "%s", "assets/scenes/m5_pillar.prefab");

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
            if (!set_path(out->scene_path, sizeof(out->scene_path), argv[++i])) {
                return false;
            }
        } else if (strcmp(arg, "--prefab") == 0 && i + 1 < argc) {
            if (!set_path(out->prefab_path, sizeof(out->prefab_path), argv[++i])) {
                return false;
            }
        } else if (strcmp(arg, "--check-pixel") == 0) {
            out->check_pixel = true;
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
//  一帧的渲染部分（主循环与像素验证共用）
// =============================================================================
//
// 抽出来的理由不是"少写三十行"，而是**像素验证必须走真实帧那条完全相同的路**：
// 一份复制出来的"检查用渲染函数"会先腐烂，然后验出一个已经不存在的行为。
//
// ---- 顺序：先跑世界，再 begin_frame，最后提交 ----
//
// 为什么 scene_submit_frame 不注册成 RENDER_SUBMIT 阶段的系统：
// renderer_begin_frame 需要一个相机，而相机要从**本帧已经算完的**世界矩阵里取。
// 提交若是系统，就必须先 begin_frame 再 update，那个相机只能是上一帧的。
// 一帧的延迟在 60fps 下看不出来，但"剔除和阴影用的是哪一帧的相机"
// 值得用一条注释解决，而不是留给下一个人猜。
typedef struct frame_state_t {
    bool shadow_fitted; // 阴影视锥要不要重新拟合（换相机/换灯之后要）
} frame_state_t;

static void render_frame(demo_t *demo, renderer_t *renderer, sprite_batch_t *batch,
                         scene_submit_t *sub, shadow_map_t *shadow, frame_state_t *state,
                         u32 width, u32 height, f64 delta, u64 frame_index)
{
    const f32 aspect = (height > 0u) ? (f32)width / (f32)height : 1.0f;
    (void)ecs_world_update(scene_world(demo->scene), (f32)delta);

    camera_t camera = camera_default();
    const bool has_camera = scene_submit_active_camera(sub, aspect, &camera);
    frame_desc_t frame;
    fill_frame_desc(&frame, width, height, delta, frame_index);
    renderer_begin_frame(renderer, &camera, &frame);

    sprite_batch_begin(batch);
    scene_submit_frame(sub, renderer, aspect);
    sprite_batch_end(batch);

    if (shadow != NULL) {
        if (!state->shadow_fitted && has_camera) {
            // 主光的朝向来自场景（存档里的那盏平行光）。
            // 第一帧提交之后 renderer 里就有了它，这时候拟合一次阴影视锥。
            const light_t *key = light_set_get(renderer_lights(renderer), 0u);
            if (key != NULL && key->cast_shadow) {
                shadow_map_fit_directional(shadow, key, vec3_make(0.0f, 1.5f, 0.0f), 22.0f);
                state->shadow_fitted = true;
            }
        }
        if (state->shadow_fitted) {
            shadow_map_render(shadow, renderer);
        }
    }
    renderer_end_frame(renderer);
}

// =============================================================================
//  M5 的两条判据：动父节点叶子跟着走 / 2D 与 3D 在同一帧里都画出来了
// =============================================================================

static entity_t find_by_name(demo_t *demo, const char *name)
{
    ecs_world_t *world = scene_world(demo->scene);
    entity_t list[512];
    const u32 count = ecs_collect_entities(world, list, 512u);
    for (u32 i = 0u; i < count; ++i) {
        const scene_name_t *n = (const scene_name_t *)ecs_peek(world, list[i], demo->ids.name);
        if (n != NULL && strcmp(n->text, name) == 0) {
            return list[i];
        }
    }
    return ENTITY_NONE;
}

static vec3_t world_position(ecs_world_t *world, entity_t e)
{
    ecs_world_transform_t wt;
    if (!ecs_transform_get_world(world, e, &wt)) {
        return vec3_make(0.0f / 0.0f, 0.0f, 0.0f); // NaN：调用方一看数字就知道没读到
    }
    return vec3_make(wt.model.col[3].x, wt.model.col[3].y, wt.model.col[3].z);
}

// 父子链：动根节点，叶子的世界位置必须在**同一帧内**移动同样的量。
//
// 这条为什么值得单独验一遍（而不只看测试）：D4 选的"拍平拓扑序"最大的风险就是
// 求值顺序 —— 顺序错了的症状是"叶子慢一帧"，在 60 FPS 下肉眼看不出来，
// 但它会让 M6 的 2.5D 挂点、M9 的动画骨骼全部错位。
// 用 dt=0 跑这两帧：自转不推进，于是两次读数之间唯一变化的就是这一次位移。
static bool verify_parent_chain(demo_t *demo)
{
    ecs_world_t *world = scene_world(demo->scene);
    const entity_t root = find_by_name(demo, "lamp_rig");
    const entity_t leaf = find_by_name(demo, "lamp_light");
    if (!entity_is_valid(root) || !entity_is_valid(leaf)) {
        LOG_ERROR("父子链验证失败：场景里找不到 lamp_rig 或 lamp_light");
        return false;
    }
    // 非轴对齐的位移：只沿一个轴动的话，"深度算错但轴向碰巧对"这类 bug 能蒙过去
    const vec3_t shift = vec3_make(3.0f, -1.5f, 2.25f);

    (void)ecs_world_update(world, 0.0f);
    const vec3_t before = world_position(world, leaf);
    const vec3_t root_before = world_position(world, root);

    ecs_transform_t *tr = (ecs_transform_t *)ecs_get(world, root, demo->ids.transform);
    if (tr == NULL) {
        LOG_ERROR("父子链验证失败：%s 没有本地变换", "lamp_rig");
        return false;
    }
    const vec3_t original = tr->position;
    const quat_t rotation = tr->rotation;
    const vec3_t scale = tr->scale;
    if (!ecs_transform_set_local(world, root, vec3_add(original, shift), rotation, scale)) {
        LOG_ERROR("父子链验证失败：移动根节点就被拒绝");
        return false;
    }
    (void)ecs_world_update(world, 0.0f);
    const vec3_t after = world_position(world, leaf);
    const vec3_t root_after = world_position(world, root);
    // 再更新一次、不改动任何东西：如果叶子这一次才跟上，说明"每帧只传播一层"，
    // 也就是拓扑序没保证父先于子 —— 那和"永远不传播"是完全不同的两个 bug。
    (void)ecs_world_update(world, 0.0f);
    const vec3_t one_more = world_position(world, leaf);

    // 验证的副作用必须还原：这条函数跑完之后，场景要和没跑过一样
    (void)ecs_transform_set_local(world, root, original, rotation, scale);
    (void)ecs_world_update(world, 0.0f);

    f32 err = math_abs((after.x - before.x) - shift.x);
    err = math_max(err, math_abs((after.y - before.y) - shift.y));
    err = math_max(err, math_abs((after.z - before.z) - shift.z));
    printf("父子链 %d 层（%s → %s）：动根 %+g/%+g/%+g → 叶子世界位置变 %+.4f/%+.4f/%+.4f，"
           "最大误差 %.1e %s\n",
           (int)(ecs_hierarchy_depth(world, leaf) + 1u), "lamp_rig", "lamp_light", (double)shift.x,
           (double)shift.y, (double)shift.z, (double)(after.x - before.x),
           (double)(after.y - before.y), (double)(after.z - before.z), (double)err,
           (err < 1e-3f) ? "（通过）" : "（不符！）");
    if (err >= 1e-3f) {
        // 分两种情况报：根自己都没动 = 世界矩阵这一趟根本没重算；
        // 根动了叶子没动 = 拓扑序或传播有问题。两者的修法完全不同，
        // 只报"验证失败"的话下一个人得从第一条断点开始查。
        LOG_ERROR("父子链验证未通过：根节点世界位置变了 %+.4f/%+.4f/%+.4f，"
                  "同一次更新里叶子变 %+.4f/%+.4f/%+.4f，再更新一次叶子变 %+.4f/%+.4f/%+.4f",
                  (double)(root_after.x - root_before.x), (double)(root_after.y - root_before.y),
                  (double)(root_after.z - root_before.z), (double)(after.x - before.x),
                  (double)(after.y - before.y), (double)(after.z - before.z),
                  (double)(one_more.x - before.x), (double)(one_more.y - before.y),
                  (double)(one_more.z - before.z));
        return false;
    }
    return ecs_hierarchy_depth(world, leaf) >= 3u;
}

// 一帧里 2D 与 3D 都画出来了 —— 用"分别拿掉一样，像素必须变"来证明。
//
// 为什么不像 M4 那样直接对颜色：精灵和方块都画在同一个场景目标里，
// 单张图上"哪个像素属于谁"说不清；而**做两次扰动**（摘掉精灵 / 挪走方块）
// 之后差异像素落在哪里，就是它们各自画在哪里的证据。
// 两次扰动的差异区几乎不重叠 = "互不干扰"（2D 盖住的地方，挪 3D 也看不出来）。
static bool check_2d_and_3d(demo_t *demo, renderer_t *renderer, sprite_batch_t *batch,
                            scene_submit_t *sub, shadow_map_t *shadow, frame_state_t *state,
                            u32 width, u32 height)
{
    if (!renderer_scene_target_enabled(renderer)) {
        LOG_ERROR("像素验证要求场景目标开着（环境里的 use_scene_target）");
        return false;
    }
    scene_t *scene = demo->scene;
    ecs_world_t *world = scene_world(scene);
    const entity_t sprite_e = find_by_name(demo, "hud_sprite");
    const entity_t cube_e = find_by_name(demo, "cube_spinning");
    if (!entity_is_valid(sprite_e) || !entity_is_valid(cube_e)) {
        LOG_ERROR("像素验证找不到 hud_sprite 或 cube_spinning");
        return false;
    }

    // 读中间那半条横带：整幅 1280×720×4 是 3.7MB，而精灵与方块都在这条带子里。
    const u32 band_y = height / 4u;
    const u32 band_h = height / 2u;
    const usize bytes = (usize)width * band_h * 4u;
    const usize pixels = (usize)width * band_h;
    arena_t *scratch = scene_scratch_arena(scene);
    const arena_mark_t mark = arena_mark(scratch);
    u8 *base = (u8 *)arena_alloc(scratch, bytes);
    u8 *probe = (u8 *)arena_alloc(scratch, bytes);
    // 每个像素一字节，记下"摘掉精灵时这里变过"。要这张表是因为"互不干扰"
    // 的判据是**两次扰动的差异区不重叠**，而两帧都留住的话内存要翻倍。
    u8 *changed_by_2d = (u8 *)arena_alloc_zero(scratch, pixels);
    bool ok = (base != NULL && probe != NULL && changed_by_2d != NULL);

    const scene_sprite_renderer_t *sr_src =
        (const scene_sprite_renderer_t *)ecs_peek(world, sprite_e, demo->ids.sprite_renderer);
    scene_sprite_renderer_t sprite_copy;
    memset(&sprite_copy, 0, sizeof(sprite_copy));
    if (ok && sr_src != NULL) {
        sprite_copy = *sr_src;
    }
    ecs_transform_t *ct = (ecs_transform_t *)ecs_get(world, cube_e, demo->ids.transform);
    vec3_t cube_original = vec3_zero();
    quat_t cube_rotation = quat_identity();
    vec3_t cube_scale = vec3_one();
    if (ok && ct != NULL) {
        cube_original = ct->position;
        cube_rotation = ct->rotation;
        cube_scale = ct->scale;
    }

    // 三次渲染都用 dt=0：相机不许在这中间偷偷转过去
    if (ok) {
        render_frame(demo, renderer, batch, sub, shadow, state, width, height, 0.0, 0ULL);
        ok = renderer_read_scene_rect(renderer, 0u, band_y, width, band_h, base);
    }
    usize diff_2d = 0u;
    if (ok) {
        (void)ecs_remove(world, sprite_e, demo->ids.sprite_renderer);
        render_frame(demo, renderer, batch, sub, shadow, state, width, height, 0.0, 1ULL);
        ok = renderer_read_scene_rect(renderer, 0u, band_y, width, band_h, probe);
        (void)ecs_add(world, sprite_e, demo->ids.sprite_renderer, &sprite_copy);
        if (ok) {
            for (usize p = 0u; p < pixels; ++p) {
                if (memcmp(base + p * 4u, probe + p * 4u, 4u) != 0) {
                    diff_2d++;
                    changed_by_2d[p] = 1u;
                }
            }
        }
    }
    usize diff_3d = 0u, overlap = 0u;
    if (ok) {
        // 把金属方块挪开 4 个单位（精灵留在原地）
        (void)ecs_transform_set_local(
            world, cube_e, vec3_add(cube_original, vec3_make(4.0f, 4.0f, 0.0f)), cube_rotation,
            cube_scale);
        render_frame(demo, renderer, batch, sub, shadow, state, width, height, 0.0, 2ULL);
        ok = renderer_read_scene_rect(renderer, 0u, band_y, width, band_h, probe);
        (void)ecs_transform_set_local(world, cube_e, cube_original, cube_rotation, cube_scale);
        if (ok) {
            for (usize p = 0u; p < pixels; ++p) {
                if (memcmp(base + p * 4u, probe + p * 4u, 4u) != 0) {
                    diff_3d++;
                    overlap += (changed_by_2d[p] != 0u) ? 1u : 0u;
                }
            }
        }
    }
    // 再验一条更贴近"动画到底有没有上屏"的：连跑两帧**真实增量**的时间，
    // 画面必须变。这一条是给本轮修的那个调度缺陷准备的 —— 世界矩阵的求值
    // 老挂在"有没有系统注册在 TRANSFORM 阶段"上，于是 Spin 在跑、计数器在涨，
    // 屏幕上却是静止的。**只有读回像素能看见这种 bug。**
    usize diff_motion = 0u;
    if (ok) {
        render_frame(demo, renderer, batch, sub, shadow, state, width, height, 1.0 / 60.0, 100ULL);
        ok = renderer_read_scene_rect(renderer, 0u, band_y, width, band_h, probe);
        memcpy(base, probe, bytes); // 以这一帧为基准
        if (ok) {
            render_frame(demo, renderer, batch, sub, shadow, state, width, height, 1.0 / 60.0,
                         101ULL);
            ok = renderer_read_scene_rect(renderer, 0u, band_y, width, band_h, probe);
        }
        if (ok) {
            for (usize p = 0u; p < pixels; ++p) {
                if (memcmp(base + p * 4u, probe + p * 4u, 4u) != 0) {
                    diff_motion++;
                }
            }
        }
    }
    arena_rewind(scratch, mark);

    const usize band_pixels = (usize)width * band_h;
    printf("同帧共存：横带 %u×%u = %llu 像素；摘掉精灵变了 %llu 个（2D），"
           "挪走方块变了 %llu 个（3D），重叠 %llu 个\n",
           (unsigned)width, (unsigned)band_h, (unsigned long long)band_pixels,
           (unsigned long long)diff_2d, (unsigned long long)diff_3d,
           (unsigned long long)overlap);
    printf("动画上屏：连续两帧真实增量，%llu 个像素变了\n", (unsigned long long)diff_motion);
    if (!ok) {
        LOG_ERROR("像素读回失败");
        return false;
    }
    // 2D/3D 各要占至少 200 个像素：少于这个数说明它压根没被画出来（或只擦了个边）
    return diff_2d >= 200u && diff_3d >= 200u && diff_motion >= 200u;
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

    // 工作目录要先定下来：日志文件、资产路径、存档里记的路径全都按它解释。
    u32        levels_up = 0u;
    const bool rooted = settle_assets_root(&levels_up);

    if (!log_init("06_scene.log", LOG_LEVEL_INFO)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }
    LOG_INFO("Polaire Engine %s —— M5 场景层", POLAIRE_VERSION_STRING);

    if (!rooted) {
        // 报一次就说清楚：后面的"资产读不到"全是这个原因，不是资产坏了。
        LOG_WARN("当前目录往上 6 层内没找到 assets/，路径将按当前目录解释："
                 "几何会回落成程序化立方体，而存盘可能失败");
    } else if (levels_up > 0u) {
        LOG_INFO("资源根不在当前目录：工作目录已向上切换 %u 级", (unsigned)levels_up);
    }

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
    window_desc.title = "Polaire Engine M5 —— 场景层";
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
    //
    // bench 的容量要留出精灵那一成：混排压测（M3/M4 的"含渲染 60 FPS"判据问的就是
    // 1 万方块 + 1000 动图精灵**同一帧**）如果按 count 卡死容量，
    // 精灵会一个都建不出来，而那看起来只是"容量到了"而不是"判据没测到"。
    u32 capacity = (options.bench > 0u)
                       ? ((options.bench > DEMO_MAX_BENCH_ENTS) ? DEMO_MAX_BENCH_ENTS
                                                                : options.bench + options.bench / 10u + 64u)
                       : 4096u;
    if (capacity > DEMO_MAX_BENCH_ENTS) {
        capacity = DEMO_MAX_BENCH_ENTS;
    }
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

    // 每次启动都验一遍"两种编码描述的是同一份数据"。失败就不往下走：
    // 这个示例是 M5 的验收载体之一，它自己报的"通过"才算数。
    if (!verify_format_equivalence(scene)) {
        LOG_ERROR("格式自检未通过，退出");
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
        run_bench(&demo, &assets, renderer, &tracker, options.bench, options.vsync);
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
    u32   frames = 0u;
    // 阴影要不要重拟合，交给 render_frame 自己记（它也是唯一用得到的地方）
    frame_state_t fstate;
    memset(&fstate, 0, sizeof(fstate));

    // ---- M5 判据的现场验证（必须有 GPU 和完整场景，单元测试是离线的）----
    if (!verify_parent_chain(&demo)) {
        LOG_ERROR("父子链验证未通过");
        goto cleanup;
    }
    if (options.check_pixel) {
        u32 width = options.width, height = options.height;
        platform_window_get_size(window, &width, &height);
        exit_code = check_2d_and_3d(&demo, renderer, &batch, &sub, shadow_ready ? &shadow : NULL,
                                    &fstate, width, height)
                        ? 0
                        : 1;
        goto cleanup;
    }

    printf("\n开始渲染。ESC 退出 / N 加柱子 / P 存盘（两种编码）/ R 重新加载 / 1、2 换编码\n");

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
            if (save_both_formats(scene, options.scene_path, &stats) &&
                verify_format_equivalence(scene)) {
                printf("已存盘（两种编码）：%u 个实体 / %u 个组件\n",
                       (unsigned)stats.entities_written, (unsigned)stats.components_written);
            }
        }
        // 重新加载有三种触发方式（R 原样重载、1 换二进制、2 换 JSON），
        // 所以清世界 + 读档 + 绑资源 + 自检收在一个函数里：
        // 抄三遍的话迟早有一遍忘了绑资源，而"忘了绑"的表现是物体全都不显示。
        bool reload_wanted = false;
        if (input_key_pressed(input, KEY_R)) {
            reload_wanted = true;
        }
        if (input_key_pressed(input, KEY_1) && scene_path_is_json(options.scene_path)) {
            char alt[512];
            sibling_format_path(options.scene_path, alt, sizeof(alt));
            if (set_path(options.scene_path, sizeof(options.scene_path), alt)) {
                printf("改用二进制那份: %s\n", options.scene_path);
                reload_wanted = true;
            }
        }
        if (input_key_pressed(input, KEY_2) && !scene_path_is_json(options.scene_path)) {
            char alt[512];
            sibling_format_path(options.scene_path, alt, sizeof(alt));
            if (set_path(options.scene_path, sizeof(options.scene_path), alt)) {
                printf("改用 JSON 那份: %s\n", options.scene_path);
                reload_wanted = true;
            }
        }
        if (reload_wanted) {
            f64 ms = 0.0;
            u32 loaded = 0u;
            if (reload_from(scene, &assets, options.scene_path, &ms, &loaded)) {
                // 换了相机（重新加载会重建实体）就要重新适配阴影贴图的正交范围
                fstate.shadow_fitted = false;
                pillar_count = 0u;
                printf("重新加载：%u 个实体 / %.2f ms\n", (unsigned)loaded, (double)ms);
            } else {
                LOG_ERROR("重新加载失败（或格式自检未通过），画面保持上一帧的世界");
            }
        }

        const f64 delta = frame_clock_tick(&clock);
        demo.last_spins = demo.spins_applied;
        demo.spins_applied = 0u;

        u32 width = options.width, height = options.height;
        platform_window_get_size(window, &width, &height);
        render_frame(&demo, renderer, &batch, &sub, shadow_ready ? &shadow : NULL, &fstate, width,
                     height, delta, (u64)frames);

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
                     "Polaire Engine M5 | %.0f fps | 实体 %u | 提交 %d 精灵 %d 剔除 %u | Spin %u | "
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
