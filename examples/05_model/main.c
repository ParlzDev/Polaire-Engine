// =============================================================================
//  examples/05_model/main.c —— M4 演示：3D 渲染管线
//
//  验证 M4 的每一条验收标准：
//    * 多光源（5 盏：1 平行光 + 2 点光 + 1 聚光 + 1 补光平行光）+ 阴影贴图
//    * 模型查看器：可旋转缩放，加载 OBJ，显示材质与光照
//    * 渲染目标抽象（场景目标是离屏 FBO，带 MSAA 解析）
//    * sRGB 管线理顺：光照在线性空间，present pass 编码回 sRGB
//      —— 用 `--srgb-check` 做端到端数值验证
//    * 视锥剔除生效（标题栏显示被剔除的物体数）
//    * 上层不直接调用 GL（本文件里搜不到 gl->）
//
//  操作：
//      ESC        退出
//      1 / 2      透视 / 正交相机
//      3          阴影开关
//      4          天空盒开关
//      5          雾开关
//      6          视锥剔除开关
//      7          色调映射：无 / Reinhard / ACES
//      8 / 9      曝光 -/+
//      C          开关离屏场景目标（对比 sRGB 管线的差别）
//      G          材质网格开关（展示粗糙度/金属度变化）
//      左键拖动   环绕相机
//      右键拖动   平移目标点
//      滚轮       缩放
//
//  命令行参数：
//      --frames N      跑 N 帧后退出（CI 用）
//      --hidden        不显示窗口
//      --no-vsync      关闭垂直同步
//      --width/--height
//      --obj PATH      加载指定的 OBJ 文件
//      --srgb-check    只做 sRGB 自检，输出结果后退出（不需要人看画面）
//      --no-shadow     启动时关闭阴影
// =============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"

// =============================================================================
//  内置兜底模型：一个立方体
//
//  找不到 assets/models/cube.obj 时用它，保证示例在任何工作目录下都能跑起来。
//  顶点带法线与 UV —— PBR 着色器依赖这两个属性。
// =============================================================================
static const char *const k_builtin_cube_obj =
    "# 内置兜底立方体（带法线与 UV）\n"
    "v -1 -1  1\n"
    "v  1 -1  1\n"
    "v  1  1  1\n"
    "v -1  1  1\n"
    "v -1 -1 -1\n"
    "v  1 -1 -1\n"
    "v  1  1 -1\n"
    "v -1  1 -1\n"
    "vt 0 0\n"
    "vt 1 0\n"
    "vt 1 1\n"
    "vt 0 1\n"
    "vn  0  0  1\n"
    "vn  0  0 -1\n"
    "vn  1  0  0\n"
    "vn -1  0  0\n"
    "vn  0  1  0\n"
    "vn  0 -1  0\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/1/1 3/3/1 4/4/1\n"
    "f 6/1/2 5/2/2 8/3/2\n"
    "f 6/1/2 8/3/2 7/4/2\n"
    "f 2/1/3 6/2/3 7/3/3\n"
    "f 2/1/3 7/3/3 3/4/3\n"
    "f 5/1/4 1/2/4 4/3/4\n"
    "f 5/1/4 4/3/4 8/4/4\n"
    "f 4/1/5 3/2/5 7/3/5\n"
    "f 4/1/5 7/3/5 8/4/5\n"
    "f 5/1/6 6/2/6 2/3/6\n"
    "f 5/1/6 2/3/6 1/4/6\n";

// =============================================================================
//  命令行
// =============================================================================
typedef struct options_t {
    u32         width;
    u32         height;
    u32         frames;
    bool        hidden;
    bool        vsync;
    bool        srgb_check;
    bool        shadow;
    bool        culling;
    const char *obj_path;
    const char *mesh_path;
    const char *dump_path;
} options_t;

static void print_usage(void)
{
    printf("用法: 05_model [选项]\n");
    printf("  --frames N     跑 N 帧后退出（0 = 一直跑，默认 0）\n");
    printf("  --hidden       创建窗口但不显示\n");
    printf("  --no-vsync     关闭垂直同步\n");
    printf("  --width N      窗口宽度（默认 1280）\n");
    printf("  --height N     窗口高度（默认 720）\n");
    printf("  --obj PATH     加载指定的 OBJ 文件\n");
    printf("  --mesh PATH    加载指定的 .mesh 二进制文件（tools/mesh_tool 转换产出）\n");
    printf("  --no-cull      启动时关闭视锥剔除（用于对比 draw call 数）\n");
    printf("  --srgb-check   只做 sRGB 自检后退出\n");
    printf("  --no-shadow    启动时关闭阴影\n");
    printf("  --dump PATH    渲染 3 帧后把画面转储成 PPM 并退出\n");
    printf("\n运行时按键: ESC 退出 / 1 透视 / 2 正交 / 3 阴影 / 4 天空盒 / 5 雾\n");
    printf("            6 剔除 / 7 色调映射 / 8,9 曝光 / C 离屏目标 / G 材质网格\n");
    printf("            左键环绕 / 右键平移 / 滚轮缩放\n");
}

static bool parse_u32(const char *text, u32 *out_value)
{
    u32 value = 0;
    if (text == NULL || *text == '\0') {
        return false;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10u + (u32)(*p - '0');
    }
    *out_value = value;
    return true;
}

static options_t parse_options(int argc, char **argv)
{
    options_t options;
    memset(&options, 0, sizeof(options));
    options.width = 1280;
    options.height = 720;
    options.frames = 0;
    options.hidden = false;
    options.vsync = true;
    options.srgb_check = false;
    options.shadow = true;
    options.culling = true;
    options.obj_path = NULL;
    options.mesh_path = NULL;
    options.dump_path = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--frames") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.frames);
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.width);
        } else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.height);
        } else if (strcmp(arg, "--obj") == 0 && i + 1 < argc) {
            options.obj_path = argv[++i];
        } else if (strcmp(arg, "--mesh") == 0 && i + 1 < argc) {
            options.mesh_path = argv[++i];
        } else if (strcmp(arg, "--no-cull") == 0) {
            options.culling = false;
        } else if (strcmp(arg, "--hidden") == 0) {
            options.hidden = true;
        } else if (strcmp(arg, "--no-vsync") == 0) {
            options.vsync = false;
        } else if (strcmp(arg, "--srgb-check") == 0) {
            options.srgb_check = true;
            options.hidden = true;
        } else if (strcmp(arg, "--dump") == 0 && i + 1 < argc) {
            options.dump_path = argv[++i];
            options.hidden = true;
            options.frames = 3; // 渲染几帧让资源与状态稳定后转储
        } else if (strcmp(arg, "--no-shadow") == 0) {
            options.shadow = false;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage();
            return options;
        } else {
            printf("未知参数: %s\n", arg);
            print_usage();
        }
    }
    return options;
}

// =============================================================================
//  场景资源
// =============================================================================
typedef struct scene_t {
    pipeline_handle_t pipeline_pbr;       // 不透明 PBR + 背面剔除
    pipeline_handle_t pipeline_pbr_two;   // 不透明 PBR + 双面（地面/薄物体）
    pipeline_handle_t pipeline_lines;     // 调试线（不投影）

    mesh_handle_t mesh_ground;
    mesh_handle_t mesh_cube;
    mesh_handle_t mesh_grid_lines;
    mesh_handle_t mesh_model; // OBJ

    texture_handle_t texture_checker;
    texture_handle_t texture_gradient;
    texture_handle_t texture_white;

    material_t material_ground;
    material_t material_model;
    material_t material_material_grid;
} scene_t;

static bool scene_build(scene_t *scene, renderer_t *renderer, const options_t *options)
{
    memset(scene, 0, sizeof(*scene));

    // ---- 着色器与管线 ----
    const shader_desc_t pbr_desc = shader_builtin_pbr_textured();
    const shader_desc_t solid_desc = shader_builtin_solid_color();

    const shader_handle_t shader_pbr = renderer_create_shader(renderer, &pbr_desc);
    const shader_handle_t shader_solid = renderer_create_shader(renderer, &solid_desc);
    if (!shader_is_valid(renderer, shader_pbr) || !shader_is_valid(renderer, shader_solid)) {
        LOG_ERROR("内置着色器创建失败");
        return false;
    }

    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_pbr;
        desc.raster.cull = CULL_MODE_BACK;
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "pbr_opaque";
        scene->pipeline_pbr = renderer_create_pipeline(renderer, &desc);
    }
    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_pbr;
        // 地面是单面片，法线朝上；相机绕到地平线以下时会看到背面，
        // 所以这里不剔除 —— 剔了会看到"地面凭空消失"。
        desc.raster.cull = CULL_MODE_NONE;
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "pbr_two_sided";
        scene->pipeline_pbr_two = renderer_create_pipeline(renderer, &desc);
    }
    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_solid;
        desc.topology = PRIMITIVE_TOPOLOGY_LINE_LIST;
        desc.raster.cull = CULL_MODE_NONE;
        desc.depth = depth_state_default();
        desc.blend = blend_state_alpha();
        desc.multisample.samples = 4;
        desc.debug_name = "debug_lines";
        scene->pipeline_lines = renderer_create_pipeline(renderer, &desc);
    }

    if (!renderer_pipeline_is_valid(renderer, scene->pipeline_pbr) ||
        !renderer_pipeline_is_valid(renderer, scene->pipeline_pbr_two) ||
        !renderer_pipeline_is_valid(renderer, scene->pipeline_lines)) {
        LOG_ERROR("管线创建失败");
        return false;
    }

    // ---- 纹理 ----
    // 检查板用中性灰阶：彩色贴图会让"光照对不对"变得难以判断。
    const u8 checker_dark[3] = {64, 64, 68};
    const u8 checker_light[3] = {196, 196, 200};
    scene->texture_checker =
        texture_create_checkerboard(renderer, 512, 64, checker_dark, checker_light);
    scene->texture_gradient = texture_create_gradient(renderer, 256, 256);
    scene->texture_white = texture_create_solid(renderer, 255, 255, 255, 255);

    if (!texture_is_valid(renderer, scene->texture_checker) ||
        !texture_is_valid(renderer, scene->texture_gradient) ||
        !texture_is_valid(renderer, scene->texture_white)) {
        LOG_ERROR("纹理创建失败");
        return false;
    }

    // ---- 网格 ----
    scene->mesh_ground = mesh_create_plane(renderer, 40.0f, 16);
    scene->mesh_cube = mesh_create_cube(renderer, 1.0f);
    scene->mesh_grid_lines = mesh_create_grid(renderer, 20.0f, 20);

    // 模型：.mesh（二进制，一次 memcpy）> --obj 指定的 OBJ > 仓库 cube.obj > 内置。
    // OBJ 是文本源格式，每次加载都要重做词法解析；.mesh 是它的预烘焙结果。
    scene->mesh_model = MESH_HANDLE_INVALID;
    if (options->mesh_path != NULL) {
        LOG_INFO("从文件加载 .mesh: %s", options->mesh_path);
        scene->mesh_model = mesh_create_from_mesh_file(renderer, options->mesh_path);
    } else if (options->obj_path != NULL) {
        LOG_INFO("从文件加载 OBJ: %s", options->obj_path);
        scene->mesh_model = mesh_create_from_obj_file(renderer, options->obj_path);
    }
    if (!mesh_is_valid(renderer, scene->mesh_model)) {
        const char *fallback = "assets/models/cube.obj";
        scene->mesh_model = mesh_create_from_obj_file(renderer, fallback);
        if (mesh_is_valid(renderer, scene->mesh_model)) {
            LOG_INFO("加载 %s 成功", fallback);
        }
    }
    if (!mesh_is_valid(renderer, scene->mesh_model)) {
        LOG_INFO("未找到 OBJ 文件，使用内置立方体");
        scene->mesh_model = mesh_create_from_obj_memory(renderer, k_builtin_cube_obj,
                                                        strlen(k_builtin_cube_obj),
                                                        "builtin:cube");
    }

    if (!mesh_is_valid(renderer, scene->mesh_ground) ||
        !mesh_is_valid(renderer, scene->mesh_cube) ||
        !mesh_is_valid(renderer, scene->mesh_grid_lines) ||
        !mesh_is_valid(renderer, scene->mesh_model)) {
        LOG_ERROR("网格创建失败");
        return false;
    }

    // ---- 材质 ----
    scene->material_ground = material_with_pipeline(scene->pipeline_pbr_two);
    material_set_texture(&scene->material_ground, RENDER_TEXTURE_SLOT_ALBEDO,
                         scene->texture_checker, NULL);

    scene->material_model = material_with_pipeline(scene->pipeline_pbr);
    material_set_texture(&scene->material_model, RENDER_TEXTURE_SLOT_ALBEDO,
                         scene->texture_gradient, NULL);

    scene->material_material_grid = material_with_pipeline(scene->pipeline_pbr);
    material_set_texture(&scene->material_material_grid, RENDER_TEXTURE_SLOT_ALBEDO,
                         scene->texture_white, NULL);

    LOG_INFO("场景就绪: %u 着色器 / %u 纹理 / %u 网格 / %u 管线", renderer_shader_count(renderer),
             renderer_texture_count(renderer), renderer_mesh_count(renderer),
             renderer_pipeline_count(renderer));
    return true;
}

static void scene_destroy(scene_t *scene, renderer_t *renderer)
{
    renderer_destroy_pipeline(renderer, scene->pipeline_pbr);
    renderer_destroy_pipeline(renderer, scene->pipeline_pbr_two);
    renderer_destroy_pipeline(renderer, scene->pipeline_lines);

    renderer_destroy_mesh(renderer, scene->mesh_ground);
    renderer_destroy_mesh(renderer, scene->mesh_cube);
    renderer_destroy_mesh(renderer, scene->mesh_grid_lines);
    renderer_destroy_mesh(renderer, scene->mesh_model);

    renderer_destroy_texture(renderer, scene->texture_checker);
    renderer_destroy_texture(renderer, scene->texture_gradient);
    renderer_destroy_texture(renderer, scene->texture_white);
}

// =============================================================================
//  光源：5 盏
//
//  刻意凑齐三种类型，验证 shader 里的多光源分支都走通了：
//      0  平行光（主光，投射阴影）—— 只有第一盏灯的阴影会被采样
//      1  平行光（补光，冷色）
//      2  点光（暖色台灯，带距离衰减）
//      3  点光（青色，另一侧）
//      4  聚光（打在模型上，验证角度衰减）
//
//  注意顺序：投射阴影的灯必须是**第一盏**，因为 M4 只有一张阴影贴图，
//  shader 里判断的是 `i == 0`。
// =============================================================================
static light_set_t build_lights(void)
{
    light_set_t set;
    light_set_clear(&set);

    // 主光：斜向下的暖色太阳
    light_t sun = light_directional(vec3_normalize(vec3_make(-0.42f, -0.78f, -0.46f)),
                                    vec3_make(1.0f, 0.96f, 0.88f), 3.0f);
    sun.cast_shadow = true;
    light_set_add(&set, &sun);

    // 补光：反方向的冷色弱光，避免背光面死黑
    light_t fill = light_directional(vec3_normalize(vec3_make(0.55f, -0.30f, 0.62f)),
                                     vec3_make(0.52f, 0.66f, 0.92f), 0.85f);
    fill.cast_shadow = false;
    light_set_add(&set, &fill);

    // 暖色点光
    light_t lamp = light_point(vec3_make(5.0f, 2.2f, 3.0f), vec3_make(1.0f, 0.70f, 0.40f), 22.0f,
                               12.0f);
    light_set_add(&set, &lamp);

    // 冷色点光
    light_t cool = light_point(vec3_make(-5.0f, 1.8f, -3.5f), vec3_make(0.35f, 0.85f, 1.0f), 18.0f,
                               12.0f);
    light_set_add(&set, &cool);

    // 聚光：从上方斜着打在模型上
    light_t spot = light_spot(vec3_make(-2.5f, 7.0f, 2.5f), vec3_normalize(vec3_make(0.4f, -1.0f, -0.4f)),
                              vec3_make(1.0f, 0.92f, 0.75f), 40.0f, 22.0f, 14.0f, 26.0f);
    light_set_add(&set, &spot);

    return set;
}

// =============================================================================
//  轨道相机
// =============================================================================
typedef struct orbit_camera_t {
    f32    yaw;
    f32    pitch;
    f32    distance;
    vec3_t target;
} orbit_camera_t;

static camera_t orbit_camera_to_camera(const orbit_camera_t *orbit, f32 aspect, bool perspective)
{
    const f32    cos_pitch = cosf(orbit->pitch);
    const vec3_t offset = vec3_make(cos_pitch * sinf(orbit->yaw), sinf(orbit->pitch),
                                    cos_pitch * cosf(orbit->yaw));
    const vec3_t position = vec3_add(orbit->target, vec3_scale(offset, orbit->distance));

    // pitch 钳制在 ±1.4 弧度（约 ±80°）：到 ±90° 时视线与 up 平行，
    // look_at 会退化，画面会在极点突然翻转。
    const quat_t rotation = quat_look_rotation(vec3_normalize(vec3_sub(orbit->target, position)),
                                               vec3_unit_y());

    if (perspective) {
        return camera_perspective(position, rotation, 55.0f, aspect, 0.1f, 300.0f);
    }
    return camera_orthographic(position, rotation, orbit->distance * 0.9f, aspect, -100.0f, 300.0f);
}

// =============================================================================
//  sRGB 自检
//
//  ---- 为什么必须做这个 ----
//
//  sRGB 管线做错的表现是"整体偏暗/偏亮"。因为**所有东西一起偏**，
//  肉眼完全无法判断对错 —— 你会以为那是"美术风格"。
//  唯一可靠的判据是：渲染一个已知的线性值，读回来和期望值比对。
//
//  ---- 检查的内容 ----
//
//  1. 离屏场景目标里存的必须是**线性**值。
//     用 clear 把场景目标刷成线性 0.21586，读回场景目标应该是 55
//     （0.21586 × 255 ≈ 55.04，8 位量化后是 55）。
//  2. 窗口帧缓冲里必须是 **sRGB 编码后**的值。
//     linear_to_srgb(0.21586) = 0.50196，对应字节 128。
//
//  第 1 步错了 -> 光照不在线性空间（或者场景目标被当成了 sRGB 格式）。
//  第 2 步错了 -> present pass 的编码有问题，或者根本没走 present pass。
//  两步都对，才说明"光照在线性空间 + 输出时编码"这条链是通的。
//
//  注意：这一节把色调映射临时关掉（TONEMAP_NONE）且曝光 = 1，
//  否则读回来的值还叠了一层映射，就不是纯 sRGB 编码了。
// =============================================================================
typedef struct srgb_probe_t {
    f32 linear;        // 写入的线性值
    u8  expected_lin;  // 场景目标里期望的字节
    u8  expected_srgb; // 窗口里期望的字节
} srgb_probe_t;

static bool srgb_check(renderer_t *renderer, u32 width, u32 height)
{
    static const srgb_probe_t probes[] = {
        {0.0f, 0, 0},
        {0.21586f, 55, 128}, // "50% 灰"（像素值 128）的线性值
        {0.5f, 128, 188},    // 线性 0.5 -> sRGB 0.7354 -> 字节 188
        {1.0f, 255, 255},
    };
    const usize probe_count = sizeof(probes) / sizeof(probes[0]);

    // 固定管线参数，让输出只取决于 sRGB 编码本身
    const tonemap_mode_t saved_tonemap = TONEMAP_NONE;
    renderer_set_tonemap(renderer, saved_tonemap, 1.0f);
    renderer_set_skybox(renderer, false, vec3_zero(), vec3_zero(), vec3_zero());
    renderer_set_fog(renderer, false, vec3_zero(), 0.0f, 1.0f, 0.0f);
    renderer_set_culling_enabled(renderer, false);
    renderer_set_scene_target_enabled(renderer, true);

    const u32 cx = width / 2u;
    const u32 cy = height / 2u;

    printf("\n========== sRGB 自检 ==========\n");
    printf("  %-10s %-10s %-12s %-12s %s\n", "线性值", "场景目标", "窗口(sRGB)", "期望", "结果");

    u32 failures = 0;

    for (usize i = 0; i < probe_count; ++i) {
        const srgb_probe_t *p = &probes[i];

        // 把场景目标刷成已知的线性灰。天空盒已关，也不提交任何几何，
        // 所以整个画面就是这个颜色。
        renderer_set_clear(renderer, true, vec4_make(p->linear, p->linear, p->linear, 1.0f), true,
                           1.0f);

        const camera_t camera =
            camera_perspective(vec3_make(0.0f, 0.0f, 5.0f), quat_identity(), 60.0f,
                               (f32)width / (f32)height, 0.1f, 100.0f);

        frame_desc_t frame_desc;
        memset(&frame_desc, 0, sizeof(frame_desc));
        frame_desc.target_width = width;
        frame_desc.target_height = height;
        frame_desc.delta_seconds = 1.0f / 60.0f;

        // 预约一次窗口像素回读 —— 必须在 begin_frame 之前，
        // 因为回读发生在 end_frame 内部（present pass 之后、swap 之前）。
        renderer_request_present_capture(renderer, cx, cy);

        renderer_begin_frame(renderer, &camera, &frame_desc);
        renderer_end_frame(renderer);

        u8 scene_pixel[4] = {0, 0, 0, 0};
        u8 present_pixel[4] = {0, 0, 0, 0};

        const bool got_scene = renderer_read_scene_pixel(renderer, cx, cy, scene_pixel);
        const bool got_present = renderer_captured_pixel(renderer, present_pixel);

        // 容差 1 字节：8 位量化 + 驱动可能的舍入差异。
        const bool scene_ok = got_scene && (scene_pixel[0] + 1u >= p->expected_lin) &&
                              (scene_pixel[0] <= p->expected_lin + 1u);
        const bool present_ok = got_present && (present_pixel[0] + 1u >= p->expected_srgb) &&
                                (present_pixel[0] <= p->expected_srgb + 1u);

        printf("  %-10.5f %-10u %-12u %-12u %s\n", (f64)p->linear, (unsigned)scene_pixel[0],
               (unsigned)present_pixel[0], (unsigned)p->expected_srgb,
               (scene_ok && present_ok) ? "通过" : "失败");

        if (!scene_ok) {
            LOG_ERROR("sRGB 自检失败（场景目标）: 线性 %.5f -> 读到 %u，期望 %u", (f64)p->linear,
                      (unsigned)scene_pixel[0], (unsigned)p->expected_lin);
            failures++;
        }
        if (!present_ok) {
            LOG_ERROR("sRGB 自检失败（窗口）: 线性 %.5f -> 读到 %u，期望 %u", (f64)p->linear,
                      (unsigned)present_pixel[0], (unsigned)p->expected_srgb);
            failures++;
        }
    }

    printf("  ------------------------------\n");
    if (failures == 0) {
        printf("  结论: 通过 —— 场景目标存线性值，窗口存 sRGB 编码值\n");
    } else {
        printf("  结论: 失败 %u 项\n", failures);
    }
    printf("================================\n\n");

    return failures == 0;
}

// =============================================================================
//  场景转储（--dump PATH）
//
//  把**刚渲染完的那一帧**的场景目标读回来写成 PPM。
//
//  为什么需要它：光照、阴影、材质这类东西"对不对"看日志是看不出来的。
//  而"我读回一个像素发现是 55"只能证明管线没断，不能证明画面是对的。
//  把整帧转储出来，才能客观地看到：
//    * 地面是不是被照亮了
//    * 立方体阵列的粗糙度/金属度梯度是不是真的在
//    * 模型有没有投出影子
//
//  调用时机：必须在 renderer_end_frame **之后**立刻调用 ——
//  场景目标（FBO）的内容会一直保留到下一帧开始，所以这时读是安全的；
//  而窗口帧缓冲的内容在 swap 之后就没了。
//
//  读回来的是**线性**值（场景目标在线性空间），所以这里用 CPU 侧的
//  pbr_tonemap_aces + pbr_linear_to_srgb 复现 present pass 的处理 ——
//  输出和窗口里看到的应当一致。这也顺带验证了 pbr.c 的 CPU 参考实现
//  和 GPU 上的 present 着色器是同一条曲线。
//
//  PPM 的 P6 格式无压缩、无依赖，任何图片查看器都认。
// =============================================================================
static bool dump_scene_target(renderer_t *renderer, const char *path, u32 width, u32 height,
                              tonemap_mode_t tonemap, f32 exposure)
{
    // 等比例缩到宽 480，保持长宽比
    const u32 out_width = (width >= 480u) ? 480u : width;
    const u32 out_height = (u32)((f64)height * (f64)out_width / (f64)width);
    if (out_width == 0 || out_height == 0) {
        return false;
    }

    // 从场景目标中心区域读回一块，再按步长抽样到输出尺寸。
    // 直接读全屏的话 1280x720 要 3.6 MB，对调试用途没必要。
    const u32 src_w = (width / out_width) * out_width;
    const u32 src_h = (height / out_height) * out_height;
    const u32 src_x = (width - src_w) / 2u;
    const u32 src_y = (height - src_h) / 2u;
    if (src_w == 0 || src_h == 0) {
        return false;
    }

    u8 *pixels = (u8 *)malloc((usize)src_w * src_h * 4u);
    if (pixels == NULL) {
        LOG_ERROR("转储缓冲区分配失败（%ux%u）", src_w, src_h);
        return false;
    }

    bool ok = renderer_read_scene_rect(renderer, src_x, src_y, src_w, src_h, pixels);
    if (!ok) {
        LOG_ERROR("场景目标读回失败");
        free(pixels);
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        LOG_ERROR("无法写入 %s", path);
        free(pixels);
        return false;
    }

    fprintf(file, "P6\n%u %u\n255\n", out_width, out_height);

    const u32 step_x = src_w / out_width;
    const u32 step_y = src_h / out_height;

    for (u32 oy = 0; oy < out_height; ++oy) {
        // GL 的行序是自下而上，这里翻过来，输出的图才是正的
        const u32 sy = src_h - 1u - oy * step_y;
        for (u32 ox = 0; ox < out_width; ++ox) {
            const u32   sx = ox * step_x;
            const u8   *src = &pixels[((usize)sy * src_w + sx) * 4u];

            vec3_t linear = vec3_make((f32)src[0] / 255.0f, (f32)src[1] / 255.0f,
                                      (f32)src[2] / 255.0f);
            linear = vec3_scale(linear, exposure);
            if (tonemap == TONEMAP_REINHARD) {
                linear = pbr_tonemap_reinhard(linear);
            } else if (tonemap == TONEMAP_ACES) {
                linear = pbr_tonemap_aces(linear);
            }

            const vec3_t srgb = pbr_linear_to_srgb(linear);
            const u8     rgb[3] = {(u8)(math_saturate(srgb.x) * 255.0f + 0.5f),
                                   (u8)(math_saturate(srgb.y) * 255.0f + 0.5f),
                                   (u8)(math_saturate(srgb.z) * 255.0f + 0.5f)};
            fwrite(rgb, 1, 3, file);
        }
    }

    fclose(file);
    free(pixels);
    LOG_INFO("场景已转储: %s（%ux%u）", path, out_width, out_height);
    return true;
}

// =============================================================================
//  main
// =============================================================================
int main(int argc, char **argv)
{
    const options_t options = parse_options(argc, argv);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    if (!log_init("05_model.log", LOG_LEVEL_DEBUG)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }

    LOG_INFO("MyEngine %s —— M4 3D 渲染管线", MYENGINE_VERSION_STRING);

    if (!platform_time_init(true)) {
        LOG_ERROR("计时系统初始化失败");
        log_shutdown();
        return 1;
    }

    int                exit_code = 1;
    platform_window_t *window = NULL;
    render_backend_t  *backend = NULL;
    renderer_t        *renderer = NULL;
    shadow_map_t       shadow;
    bool               shadow_ready = false;
    scene_t            scene;
    bool               scene_ready = false;
    u32                frames_rendered = 0;
    bool               srgb_ok = true;
    bool               srgb_ran = false;

    platform_window_desc_t window_desc = platform_window_desc_default();
    window_desc.title = "MyEngine M4 —— 3D 渲染管线";
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
    backend_desc.debug_name = "main";

    if (!render_backend_init(backend, &backend_desc)) {
        LOG_ERROR("渲染后端初始化失败");
        goto cleanup;
    }

    render_backend_caps_t caps;
    render_backend_get_caps(backend, &caps);
    LOG_INFO("后端: %s | %s", caps.api_name, caps.device_name);

    renderer = renderer_create(&tracker.base, backend);
    if (renderer == NULL) {
        LOG_ERROR("渲染器创建失败");
        goto cleanup;
    }

    // ---- 渲染器设置 ----
    renderer_set_ambient(renderer, vec3_make(0.55f, 0.62f, 0.78f), 0.22f);
    renderer_set_clear(renderer, true, vec4_make(0.04f, 0.05f, 0.08f, 1.0f), true, 1.0f);
    renderer_set_tonemap(renderer, TONEMAP_ACES, 1.0f);
    renderer_set_msaa_samples(renderer, 4);
    renderer_set_scene_target_enabled(renderer, true);
    renderer_set_culling_enabled(renderer, options.culling);

    // 天空盒（程序化渐变，不需要 cubemap 资源）
    renderer_set_skybox(renderer, true, vec3_make(0.66f, 0.75f, 0.88f), vec3_make(0.18f, 0.34f, 0.66f),
                        vec3_make(0.13f, 0.12f, 0.11f));

    // 雾：指数平方衰减。起始/结束距离按场景尺度取。
    renderer_set_fog(renderer, true, vec3_make(0.62f, 0.72f, 0.86f), 18.0f, 90.0f, 1.0f);

    // 光照
    const light_set_t lights = build_lights();
    renderer_set_lights(renderer, &lights);

    // ---- 阴影 ----
    if (options.shadow) {
        if (!shadow_map_init(&shadow, renderer, 2048)) {
            LOG_WARN("阴影贴图创建失败，将以无阴影模式继续");
        } else {
            shadow_ready = true;
            shadow_map_set_bias(&shadow, 2.5f, 1.8f, 0.035f);
            shadow_map_set_strength(&shadow, 0.78f);
            // 紧凑正交视锥：包住整个演示场景的包围球。
            // 球对光源方向不敏感 —— 用 AABB 的话光源每转一点阴影边缘就会抖。
            shadow_map_fit_directional(&shadow, &lights.lights[0], vec3_make(0.0f, 0.5f, 0.0f),
                                       13.0f);
        }
    }

    if (!scene_build(&scene, renderer, &options)) {
        LOG_ERROR("场景构建失败");
        goto cleanup;
    }
    scene_ready = true;

    // ---- sRGB 自检模式：跑完就退出 ----
    if (options.srgb_check) {
        u32 width = 0;
        u32 height = 0;
        platform_window_get_size(window, &width, &height);
        if (width == 0 || height == 0) {
            width = options.width;
            height = options.height;
        }
        srgb_ok = srgb_check(renderer, width, height);
        srgb_ran = true;
        exit_code = srgb_ok ? 0 : 1;
        goto cleanup;
    }

    // ---- 帧循环 ----
    orbit_camera_t orbit;
    orbit.yaw = 0.7f;
    orbit.pitch = 0.32f;
    orbit.distance = 11.0f;
    orbit.target = vec3_make(0.0f, 0.8f, 0.0f);

    bool show_perspective = true;
    bool shadows_enabled = options.shadow;
    bool skybox_enabled = true;
    bool fog_enabled = true;
    bool culling_enabled = options.culling;
    bool show_material_grid = true;
    tonemap_mode_t tonemap = TONEMAP_ACES;
    f32            exposure = 1.0f;
    bool           dragging = false;
    bool           panning = false;

    frame_clock_t   clock;
    frame_limiter_t limiter;
    frame_clock_init(&clock);
    frame_limiter_init(&limiter, 60.0);
    if (options.vsync) {
        limiter.enabled = false;
    }
    frame_clock_reset(&clock);

    char title[320];
    f64  last_title_update = 0.0;
    f32  spin = 0.0f;

    while (true) {
        const platform_time_point_t frame_start = platform_time_now();

        if (!platform_window_poll_events(window)) {
            break;
        }

        const input_state_t *input = platform_window_input(window);
        if (input_key_pressed(input, KEY_ESCAPE)) {
            break;
        }

        // ---- 交互 ----
        if (input_key_pressed(input, KEY_1)) {
            show_perspective = true;
            LOG_INFO("相机: 透视");
        }
        if (input_key_pressed(input, KEY_2)) {
            show_perspective = false;
            LOG_INFO("相机: 正交");
        }
        if (input_key_pressed(input, KEY_3)) {
            shadows_enabled = !shadows_enabled;
            LOG_INFO("阴影: %s", shadows_enabled ? "开" : "关");
        }
        if (input_key_pressed(input, KEY_4)) {
            skybox_enabled = !skybox_enabled;
            LOG_INFO("天空盒: %s", skybox_enabled ? "开" : "关");
        }
        if (input_key_pressed(input, KEY_5)) {
            fog_enabled = !fog_enabled;
            LOG_INFO("雾: %s", fog_enabled ? "开" : "关");
        }
        if (input_key_pressed(input, KEY_6)) {
            culling_enabled = !culling_enabled;
            LOG_INFO("视锥剔除: %s", culling_enabled ? "开" : "关");
        }
        if (input_key_pressed(input, KEY_7)) {
            tonemap = (tonemap_mode_t)(((int)tonemap + 1) % 3);
            static const char *const names[] = {"无", "Reinhard", "ACES"};
            LOG_INFO("色调映射: %s", names[(int)tonemap]);
        }
        if (input_key_pressed(input, KEY_8)) {
            exposure = math_clamp(exposure * 0.85f, 0.1f, 8.0f);
            LOG_INFO("曝光: %.2f", (f64)exposure);
        }
        if (input_key_pressed(input, KEY_9)) {
            exposure = math_clamp(exposure / 0.85f, 0.1f, 8.0f);
            LOG_INFO("曝光: %.2f", (f64)exposure);
        }
        if (input_key_pressed(input, KEY_G)) {
            show_material_grid = !show_material_grid;
            LOG_INFO("材质网格: %s", show_material_grid ? "开" : "关");
        }

        if (input_mouse_pressed(input, MOUSE_BUTTON_LEFT)) {
            dragging = true;
        }
        if (input_mouse_released(input, MOUSE_BUTTON_LEFT)) {
            dragging = false;
        }
        if (input_mouse_pressed(input, MOUSE_BUTTON_RIGHT)) {
            panning = true;
        }
        if (input_mouse_released(input, MOUSE_BUTTON_RIGHT)) {
            panning = false;
        }

        if (dragging) {
            orbit.yaw -= input->mouse_dx * 0.01f;
            // pitch 钳制：到 ±90° 视线与 up 平行，look_at 退化、画面翻转
            orbit.pitch = math_clamp(orbit.pitch - input->mouse_dy * 0.01f, -1.40f, 1.40f);
        }
        if (panning) {
            // 沿相机的右/上方向平移目标点，手感与"抓住场景拖动"一致
            const f32    cos_pitch = cosf(orbit.pitch);
            const vec3_t forward = vec3_make(cos_pitch * sinf(orbit.yaw), sinf(orbit.pitch),
                                             cos_pitch * cosf(orbit.yaw));
            const vec3_t right = vec3_normalize(vec3_cross(vec3_unit_y(), forward));
            const vec3_t up = vec3_normalize(vec3_cross(forward, right));
            const f32    scale = orbit.distance * 0.0015f;

            orbit.target = vec3_add(orbit.target,
                                    vec3_add(vec3_scale(right, -input->mouse_dx * scale),
                                             vec3_scale(up, input->mouse_dy * scale)));
        }
        if (input->scroll_dy != 0.0f) {
            orbit.distance = math_clamp(orbit.distance * (1.0f - input->scroll_dy * 0.12f), 2.0f,
                                        80.0f);
        }

        const f64 delta = frame_clock_tick(&clock);
        spin += (f32)delta;

        u32 width = 0;
        u32 height = 0;
        platform_window_get_size(window, &width, &height);
        if (width == 0 || height == 0) {
            frame_limiter_wait(&limiter, frame_start);
            continue;
        }

        // ---- 每帧设置 ----
        renderer_set_skybox(renderer, skybox_enabled, vec3_make(0.66f, 0.75f, 0.88f),
                            vec3_make(0.18f, 0.34f, 0.66f), vec3_make(0.13f, 0.12f, 0.11f));
        renderer_set_fog(renderer, fog_enabled, vec3_make(0.62f, 0.72f, 0.86f), 18.0f, 90.0f, 1.0f);
        renderer_set_tonemap(renderer, tonemap, exposure);
        renderer_set_culling_enabled(renderer, culling_enabled);

        // 阴影参数必须在 begin_frame **之前**写进 frame uniform ——
        // begin_frame 会把整块 uniform 上传一次。
        if (shadow_ready) {
            shadow_map_set_strength(&shadow, shadows_enabled ? 0.78f : 0.0f);
            shadow_map_fit_directional(&shadow, &lights.lights[0], vec3_make(0.0f, 0.5f, 0.0f),
                                       13.0f);
            shadow_map_apply(&shadow, renderer);
        }

        const f32    aspect = (f32)width / (f32)height;
        const camera_t camera = orbit_camera_to_camera(&orbit, aspect, show_perspective);

        frame_desc_t frame_desc;
        memset(&frame_desc, 0, sizeof(frame_desc));
        frame_desc.target_width = width;
        frame_desc.target_height = height;
        frame_desc.delta_seconds = (f32)delta;
        frame_desc.elapsed_seconds = clock.elapsed_seconds;

        renderer_begin_frame(renderer, &camera, &frame_desc);

        // 调试网格线：**不投影**（cast_shadow = false）。
        // 它是 LINE_LIST 拓扑，用三角形深度管线画会往阴影贴图里塞垃圾三角形。
        if (show_material_grid) {
            render_item_t item = render_item_default();
            item.pipeline = scene.pipeline_lines;
            item.mesh = scene.mesh_grid_lines;
            item.layer = RENDER_LAYER_BACKGROUND;
            item.cast_shadow = false;
            item.uniforms = object_uniforms_from_model(mat4_translation(vec3_make(0.0f, 0.01f, 0.0f)));
            item.uniforms.tint = vec4_make(0.42f, 0.50f, 0.62f, 0.30f);
            renderer_push_item(renderer, &item);
        }

        // 地面
        renderer_push_mesh(renderer, &scene.material_ground, scene.mesh_ground,
                           mat4_translation(vec3_make(0.0f, 0.0f, 0.0f)), RENDER_LAYER_OPAQUE_3D);

        // 中央的 OBJ 模型（缓慢自转）
        {
            const mat4_t model =
                mat4_from_trs(vec3_make(0.0f, 1.6f, 0.0f), quat_rotation_y(spin * 0.5f),
                              vec3_splat(1.5f));
            renderer_push_mesh(renderer, &scene.material_model, scene.mesh_model, model,
                               RENDER_LAYER_OPAQUE_3D);
        }

        // 材质网格：9x7 的立方体阵列，粗糙度沿 X 变化、金属度沿 Z 变化。
        //
        // 两个作用：
        //   1. 一眼看出 PBR 参数是不是真的起作用了 —— 全部一样就说明参数没传进去；
        //   2. 阵列铺得比视野宽，于是**视锥剔除**能真的剔掉东西。
        //      按 6 关掉剔除，标题栏的"绘制"数会立刻变大 ——
        //      "画得更快"这件事如果没有数字支撑，很容易是错觉。
        if (show_material_grid) {
            for (u32 ix = 0; ix < 9; ++ix) {
                for (u32 iz = 0; iz < 7; ++iz) {
                    const f32 roughness = 0.08f + (f32)(ix % 5) * 0.22f;
                    const f32 metallic = (f32)(iz % 4) / 3.0f;

                    // 从基础材质拷一份改参数 —— material_t 是值类型，这样最直观，
                    // 也不会污染其它物体。
                    material_t material = scene.material_material_grid;
                    material_set_pbr(&material, metallic, roughness, PBR_DIELECTRIC_F0, 1.0f);

                    const mat4_t model = mat4_from_trs(
                        vec3_make(-8.8f + (f32)ix * 2.2f, 0.55f, -7.7f + (f32)iz * 2.2f),
                        quat_identity(), vec3_splat(0.6f));

                    renderer_push_mesh(renderer, &material, scene.mesh_cube, model,
                                       RENDER_LAYER_OPAQUE_3D);
                }
            }
        }

        // 阴影深度 pass：必须在所有 push 之后、end_frame 之前。
        if (shadow_ready && shadows_enabled) {
            shadow_map_render(&shadow, renderer);
        }

        renderer_end_frame(renderer);

        frames_rendered++;

        // 转储：必须在 end_frame 之后立刻做（场景目标的内容还在）。
        if (options.dump_path != NULL && frames_rendered >= options.frames) {
            dump_scene_target(renderer, options.dump_path, width, height, tonemap, exposure);
            exit_code = 0;
            break;
        }

        if (clock.elapsed_seconds - last_title_update > 0.25) {
            last_title_update = clock.elapsed_seconds;

            render_stats_t stats;
            renderer_get_stats(renderer, &stats);

            snprintf(title, sizeof(title),
                     "MyEngine M4 | %.1f FPS | %.2f ms | 绘制 %u | 三角面 %u | 剔除 %u | 光源 %u | "
                     "阴影%s 天空%s 雾%s 剔除%s 曝光%.2f",
                     clock.fps, clock.delta_ms, stats.draw_calls, stats.triangles,
                     renderer_culled_count(renderer), light_set_count(&lights),
                     shadows_enabled ? "开" : "关", skybox_enabled ? "开" : "关",
                     fog_enabled ? "开" : "关", culling_enabled ? "开" : "关", (f64)exposure);
            platform_window_set_title(window, title);
        }

        if (options.frames > 0 && frames_rendered >= options.frames) {
            LOG_INFO("已达到指定帧数 %u，退出", options.frames);
            break;
        }

        frame_limiter_wait(&limiter, frame_start);
    }

    // ---- 统计 ----
    {
        render_stats_t stats;
        renderer_get_stats(renderer, &stats);
        LOG_INFO("共渲染 %u 帧，总时长 %.3f 秒", frames_rendered, clock.elapsed_seconds);
        LOG_INFO("最后一帧: 绘制调用 %u | 三角面 %u | 顶点 %u | 管线切换 %u | 纹理绑定 %u",
                 stats.draw_calls, stats.triangles, stats.vertices, stats.pipeline_switches,
                 stats.texture_bindings);
        LOG_INFO("视锥剔除: 本帧剔除 %u 个物体", renderer_culled_count(renderer));
        LOG_INFO("帧时间: 平均 %.3f ms | 最大 %.3f ms | 平滑 FPS %.1f", clock.delta_ms,
                 clock.max_delta_ms, clock.fps);
    }

    exit_code = 0;

cleanup:
    if (scene_ready) {
        scene_destroy(&scene, renderer);
    }
    if (shadow_ready) {
        shadow_map_shutdown(&shadow, renderer);
    }

    renderer_destroy(renderer);
    gl_backend_destroy(backend);
    platform_window_destroy(window);
    platform_time_shutdown();

    const bool leaked = allocator_tracking_has_leak(&tracker);
    if (leaked) {
        LOG_ERROR("内存泄漏: 仍有 %llu 块 / %llu 字节未释放",
                  (unsigned long long)tracker.live_blocks,
                  (unsigned long long)tracker.bytes_in_use);
    } else {
        LOG_INFO("零泄漏：%llu 字节全部归还", (unsigned long long)tracker.total_bytes);
    }

    const u64 log_lines = log_lines_written();
    log_shutdown();

    printf("\n");
    printf("========== M4 验收 ==========\n");
    printf("  窗口 / 后端 / 渲染器 : %s\n", exit_code == 0 ? "创建成功" : "失败");
    printf("  渲染帧数             : %u\n", frames_rendered);
    if (srgb_ran) {
        printf("  sRGB 自检            : %s\n", srgb_ok ? "通过" : "失败");
    }
    printf("  内存泄漏             : %s\n", leaked ? "有" : "无");
    printf("  日志行数             : %llu\n", (unsigned long long)log_lines);
    printf("=============================\n");

    return (exit_code == 0 && !leaked) ? 0 : 1;
}
