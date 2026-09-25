// =============================================================================
//  examples/02_render/main.c —— M2 演示：渲染核心
//
//  验证 M2 的每一条验收标准：
//    * 能画三角形（网格地面）、贴图 quad、加载 OBJ 模型
//    * 相机可切换正交 / 透视
//    * 资源用句柄，上层**不直接调用任何 GL 函数**（本文件里搜不到 gl->）
//    * 帧内零 malloc（帧 arena 稳态零系统分配）
//    * 退出时零资源泄漏、零 GL 错误
//
//  操作：
//      ESC        退出
//      1 / 2      透视 / 正交相机
//      3          线框模式
//      4          显示/隐藏参考网格
//      左键拖动   环绕相机
//      滚轮       缩放
//
//  命令行参数：
//      --frames N      跑 N 帧后退出（CI 用）
//      --hidden        不显示窗口
//      --no-vsync      关闭垂直同步
//      --width/--height
//      --obj PATH      加载指定的 OBJ 文件（默认用内置的八面体）
// =============================================================================
#include <stdio.h>
#include <string.h>

#include "engine/engine.h"

// =============================================================================
//  内置 OBJ：一个八面体
//
//  故意用 "f v//vn" 这种"有法线没 UV"的写法，覆盖 OBJ 解析器里
//  最容易出错的一条分支（三个字段里缺中间那个）。
//  同时它包含 8 个三角形面，能验证三角化与索引去重。
// =============================================================================
static const char *const k_builtin_obj =
    "# MyEngine 内置测试模型：八面体\n"
    "# 用于验证 OBJ 解析（v//vn 索引写法、负索引、索引去重）\n"
    "v  0.0  1.0  0.0\n"
    "v  0.0 -1.0  0.0\n"
    "v  1.0  0.0  0.0\n"
    "v -1.0  0.0  0.0\n"
    "v  0.0  0.0  1.0\n"
    "v  0.0  0.0 -1.0\n"
    "vn  0.5774  0.5774  0.5774\n"
    "vn -0.5774  0.5774  0.5774\n"
    "vn -0.5774  0.5774 -0.5774\n"
    "vn  0.5774  0.5774 -0.5774\n"
    "vn  0.5774 -0.5774  0.5774\n"
    "vn -0.5774 -0.5774  0.5774\n"
    "vn -0.5774 -0.5774 -0.5774\n"
    "vn  0.5774 -0.5774 -0.5774\n"
    "f 1//1 3//1 5//1\n"
    "f 1//2 5//2 4//2\n"
    "f 1//3 4//3 6//3\n"
    "f 1//4 6//4 3//4\n"
    "f 2//5 5//5 3//5\n"
    "f 2//6 4//6 5//6\n"
    "f 2//7 6//7 4//7\n"
    "f 2//8 3//8 6//8\n";

// =============================================================================
//  命令行
// =============================================================================
typedef struct options_t {
    u32  width;
    u32  height;
    u32  frames;
    bool hidden;
    bool vsync;
    const char *obj_path;
} options_t;

static void print_usage(void)
{
    printf("用法: 02_render [选项]\n");
    printf("  --frames N     跑 N 帧后退出（0 = 一直跑，默认 0）\n");
    printf("  --hidden       创建窗口但不显示\n");
    printf("  --no-vsync     关闭垂直同步\n");
    printf("  --width N      窗口宽度（默认 1280）\n");
    printf("  --height N     窗口高度（默认 720）\n");
    printf("  --obj PATH     加载指定的 OBJ 文件\n");
    printf("\n运行时按键: ESC 退出 / 1 透视 / 2 正交 / 3 线框 / 4 网格 / 左键拖动环绕 / 滚轮缩放\n");
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
    options.obj_path = NULL;

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
        } else if (strcmp(arg, "--hidden") == 0) {
            options.hidden = true;
        } else if (strcmp(arg, "--no-vsync") == 0) {
            options.vsync = false;
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
    // 管线
    pipeline_handle_t pipeline_lit;        // 不透明 + 背面剔除
    pipeline_handle_t pipeline_lit_two_sided; // 不透明 + 双面
    pipeline_handle_t pipeline_unlit;      // 无光照 + 双面
    pipeline_handle_t pipeline_wireframe;  // 线框
    pipeline_handle_t pipeline_lines;      // 参考网格线

    // 网格
    mesh_handle_t mesh_plane;
    mesh_handle_t mesh_cube;
    mesh_handle_t mesh_quad;
    mesh_handle_t mesh_grid;
    mesh_handle_t mesh_obj;

    // 纹理
    texture_handle_t texture_checker;
    texture_handle_t texture_gradient;

    // 材质
    material_t material_ground;
    material_t material_cube;
    material_t material_quad;
    material_t material_obj;
} scene_t;

static bool scene_build(scene_t *scene, renderer_t *renderer, const options_t *options)
{
    memset(scene, 0, sizeof(*scene));

    // ---- 着色器与管线 ----
    // 先存成变量再取地址：C 里不能对"函数返回的结构体"直接取地址
    // （那是右值，而且复合字面量的写法在 -Wpedantic 下也不合法）。
    const shader_desc_t lit_desc = shader_builtin_lit_textured();
    const shader_desc_t unlit_desc = shader_builtin_unlit_textured();
    const shader_desc_t solid_desc = shader_builtin_solid_color();

    const shader_handle_t shader_lit = renderer_create_shader(renderer, &lit_desc);
    const shader_handle_t shader_unlit = renderer_create_shader(renderer, &unlit_desc);
    const shader_handle_t shader_solid = renderer_create_shader(renderer, &solid_desc);

    if (!shader_is_valid(renderer, shader_lit) || !shader_is_valid(renderer, shader_unlit) ||
        !shader_is_valid(renderer, shader_solid)) {
        LOG_ERROR("内置着色器创建失败");
        return false;
    }

    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_lit;
        desc.raster.cull = CULL_MODE_BACK;
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "lit_opaque";
        scene->pipeline_lit = renderer_create_pipeline(renderer, &desc);
    }
    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_lit;
        desc.raster.cull = CULL_MODE_NONE; // 薄物体（平面/quad）要双面可见
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "lit_two_sided";
        scene->pipeline_lit_two_sided = renderer_create_pipeline(renderer, &desc);
    }
    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_unlit;
        desc.raster.cull = CULL_MODE_NONE;
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "unlit";
        scene->pipeline_unlit = renderer_create_pipeline(renderer, &desc);
    }
    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_solid;
        desc.raster = raster_state_wireframe();
        desc.raster.cull = CULL_MODE_NONE;
        desc.depth = depth_state_default();
        desc.blend = blend_state_opaque();
        desc.multisample.samples = 4;
        desc.debug_name = "wireframe";
        scene->pipeline_wireframe = renderer_create_pipeline(renderer, &desc);
    }
    {
        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shader_solid;
        desc.topology = PRIMITIVE_TOPOLOGY_LINE_LIST;
        desc.raster.cull = CULL_MODE_NONE;
        desc.depth = depth_state_default();
        desc.blend = blend_state_alpha();
        desc.multisample.samples = 4;
        desc.debug_name = "lines";
        scene->pipeline_lines = renderer_create_pipeline(renderer, &desc);
    }

    if (!renderer_pipeline_is_valid(renderer, scene->pipeline_lit) ||
        !renderer_pipeline_is_valid(renderer, scene->pipeline_lit_two_sided) ||
        !renderer_pipeline_is_valid(renderer, scene->pipeline_unlit) ||
        !renderer_pipeline_is_valid(renderer, scene->pipeline_wireframe) ||
        !renderer_pipeline_is_valid(renderer, scene->pipeline_lines)) {
        LOG_ERROR("管线创建失败");
        return false;
    }
    // ---- 纹理 ----
    const u8 checker_dark[3] = {40, 44, 52};
    const u8 checker_light[3] = {200, 205, 215};
    scene->texture_checker = texture_create_checkerboard(renderer, 256, 32, checker_dark,
                                                         checker_light);
    scene->texture_gradient = texture_create_gradient(renderer, 256, 256);

    if (!texture_is_valid(renderer, scene->texture_checker) ||
        !texture_is_valid(renderer, scene->texture_gradient)) {
        LOG_ERROR("纹理创建失败");
        return false;
    }

    // ---- 网格 ----
    scene->mesh_plane = mesh_create_plane(renderer, 24.0f, 24);
    scene->mesh_cube = mesh_create_cube(renderer, 2.0f);
    scene->mesh_quad = mesh_create_quad(renderer, 3.0f, 2.0f);
    scene->mesh_grid = mesh_create_grid(renderer, 24.0f, 24);

    if (options->obj_path != NULL) {
        LOG_INFO("从文件加载 OBJ: %s", options->obj_path);
        scene->mesh_obj = mesh_create_from_obj_file(renderer, options->obj_path);
    } else {
        scene->mesh_obj = mesh_create_from_obj_memory(renderer, k_builtin_obj,
                                                      strlen(k_builtin_obj), "builtin:octahedron");
    }

    if (!mesh_is_valid(renderer, scene->mesh_plane) || !mesh_is_valid(renderer, scene->mesh_cube) ||
        !mesh_is_valid(renderer, scene->mesh_quad) || !mesh_is_valid(renderer, scene->mesh_grid) ||
        !mesh_is_valid(renderer, scene->mesh_obj)) {
        LOG_ERROR("网格创建失败");
        return false;
    }

    // ---- 材质 ----
    scene->material_ground = material_with_pipeline(scene->pipeline_lit_two_sided);
    material_set_texture(&scene->material_ground, RENDER_TEXTURE_SLOT_ALBEDO,
                         scene->texture_checker, NULL);

    scene->material_cube = material_with_pipeline(scene->pipeline_lit);
    material_set_texture(&scene->material_cube, RENDER_TEXTURE_SLOT_ALBEDO, scene->texture_checker,
                         NULL);

    scene->material_quad = material_with_pipeline(scene->pipeline_unlit);
    material_set_texture(&scene->material_quad, RENDER_TEXTURE_SLOT_ALBEDO, scene->texture_gradient,
                         NULL);

    scene->material_obj = material_with_pipeline(scene->pipeline_lit);
    material_set_texture(&scene->material_obj, RENDER_TEXTURE_SLOT_ALBEDO, scene->texture_gradient,
                         NULL);
    scene->material_obj.tint = vec4_make(1.0f, 0.85f, 0.55f, 1.0f);

    LOG_INFO("场景就绪: %u 着色器 / %u 纹理 / %u 网格 / %u 管线",
             renderer_shader_count(renderer), renderer_texture_count(renderer),
             renderer_mesh_count(renderer), renderer_pipeline_count(renderer));
    return true;
}

// =============================================================================
//  相机轨道控制
// =============================================================================
typedef struct orbit_camera_t {
    f32 yaw;
    f32 pitch;
    f32 distance;
    vec3_t target;
} orbit_camera_t;

static camera_t orbit_camera_to_camera(const orbit_camera_t *orbit, f32 aspect,
                                       bool perspective)
{
    const f32 cos_pitch = cosf(orbit->pitch);
    const vec3_t offset = vec3_make(cos_pitch * sinf(orbit->yaw), sinf(orbit->pitch),
                                    cos_pitch * cosf(orbit->yaw));
    const vec3_t position = vec3_add(orbit->target, vec3_scale(offset, orbit->distance));

    quat_t rotation = quat_look_rotation(vec3_normalize(vec3_sub(orbit->target, position)),
                                         vec3_unit_y());

    if (perspective) {
        return camera_perspective(position, rotation, 55.0f, aspect, 0.1f, 200.0f);
    }

    // 正交：可见高度跟随轨道距离，这样"缩放"在两台相机上观感一致
    return camera_orthographic(position, rotation, orbit->distance * 0.9f, aspect, -100.0f,
                               200.0f);
}

// =============================================================================
//  main
// =============================================================================
int main(int argc, char **argv)
{
    const options_t options = parse_options(argc, argv);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    if (!log_init("02_render.log", LOG_LEVEL_DEBUG)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }

    LOG_INFO("MyEngine %s —— M2 渲染核心", MYENGINE_VERSION_STRING);

    if (!platform_time_init(true)) {
        LOG_ERROR("计时系统初始化失败");
        log_shutdown();
        return 1;
    }

    int                exit_code = 1;
    platform_window_t *window = NULL;
    render_backend_t  *backend = NULL;
    renderer_t        *renderer = NULL;
    // frames_rendered 必须声明在所有 goto 之前并初始化：
    // 否则 cleanup 标签用到它时，clang 会报 -Wsometimes-uninitialized。
    u32                frames_rendered = 0;
    scene_t            scene;
    bool               scene_ready = false;

    // ---- 窗口 ----
    platform_window_desc_t window_desc = platform_window_desc_default();
    window_desc.title = "MyEngine M2 —— 渲染核心";
    window_desc.width = options.width;
    window_desc.height = options.height;
    window_desc.visible = !options.hidden;
    window_desc.resizable = true;

    if (!platform_window_create(&tracker.base, &window_desc, &window)) {
        LOG_ERROR("窗口创建失败");
        goto cleanup;
    }

    // ---- 后端 ----
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

    // ---- 渲染器 ----
    renderer = renderer_create(&tracker.base, backend);
    if (renderer == NULL) {
        LOG_ERROR("渲染器创建失败");
        goto cleanup;
    }

    if (!scene_build(&scene, renderer, &options)) {
        LOG_ERROR("场景构建失败");
        goto cleanup;
    }
    scene_ready = true;

    // ---- 帧循环 ----
    orbit_camera_t orbit;
    orbit.yaw = 0.6f;
    orbit.pitch = 0.35f;
    orbit.distance = 12.0f;
    orbit.target = vec3_make(0.0f, 0.0f, 0.0f);

    bool show_perspective = true;
    bool wireframe = false;
    bool show_grid = true;
    bool dragging = false;

    frame_clock_t   clock;
    frame_limiter_t limiter;
    frame_clock_init(&clock);
    frame_limiter_init(&limiter, 60.0);
    if (options.vsync) {
        limiter.enabled = false;
    }

    renderer_set_ambient(renderer, vec3_make(0.55f, 0.62f, 0.78f), 0.35f);
    renderer_set_directional_light(renderer, vec3_normalize(vec3_make(-0.6f, -1.0f, -0.4f)),
                                   vec3_make(1.0f, 0.96f, 0.88f), 1.1f);
    renderer_set_clear(renderer, true, vec4_make(0.05f, 0.07f, 0.11f, 1.0f), true, 1.0f);

    frame_clock_reset(&clock);

    char title[256];
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
            wireframe = !wireframe;
            LOG_INFO("线框模式: %s", wireframe ? "开" : "关");
        }
        if (input_key_pressed(input, KEY_4)) {
            show_grid = !show_grid;
        }

        if (input_mouse_pressed(input, MOUSE_BUTTON_LEFT)) {
            dragging = true;
        }
        if (input_mouse_released(input, MOUSE_BUTTON_LEFT)) {
            dragging = false;
        }
        if (dragging) {
            orbit.yaw -= input->mouse_dx * 0.01f;
            orbit.pitch = math_clamp(orbit.pitch - input->mouse_dy * 0.01f, -1.4f, 1.4f);
        }
        if (input->scroll_dy != 0.0f) {
            orbit.distance = math_clamp(orbit.distance * (1.0f - input->scroll_dy * 0.12f), 2.0f,
                                        60.0f);
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

        const f32 aspect = (f32)width / (f32)height;
        const camera_t camera = orbit_camera_to_camera(&orbit, aspect, show_perspective);

        // ---- 一帧 ----
        frame_desc_t frame_desc;
        memset(&frame_desc, 0, sizeof(frame_desc));
        frame_desc.target_width = width;
        frame_desc.target_height = height;
        frame_desc.delta_seconds = (f32)delta;
        frame_desc.elapsed_seconds = clock.elapsed_seconds;

        renderer_begin_frame(renderer, &camera, &frame_desc);

        // 参考网格（线条，放在最底层）
        if (show_grid) {
            render_item_t item = render_item_default();
            item.pipeline = scene.pipeline_lines;
            item.mesh = scene.mesh_grid;
            item.layer = RENDER_LAYER_BACKGROUND;
            item.uniforms = object_uniforms_from_model(mat4_translation(vec3_make(0.0f, -1.0f, 0.0f)));
            item.uniforms.tint = vec4_make(0.28f, 0.34f, 0.44f, 0.55f);
            renderer_push_item(renderer, &item);
        }

        // 地面
        renderer_push_mesh(renderer, &scene.material_ground, scene.mesh_plane,
                           mat4_translation(vec3_make(0.0f, -1.0f, 0.0f)), RENDER_LAYER_OPAQUE_3D);

        // 旋转的立方体
        {
            const mat4_t model = mat4_from_trs(
                vec3_make(0.0f, 0.6f, 0.0f),
                quat_mul(quat_rotation_y(spin * 0.7f), quat_rotation_x(spin * 0.45f)),
                vec3_one());
            renderer_push_mesh(renderer, &scene.material_cube, scene.mesh_cube, model,
                               RENDER_LAYER_OPAQUE_3D);
        }

        // 悬浮的贴图 quad（无光照）
        {
            const mat4_t model =
                mat4_from_trs(vec3_make(-4.2f, 1.4f + sinf(spin * 1.3f) * 0.25f, 0.0f),
                              quat_rotation_y(spin * 0.9f), vec3_one());
            renderer_push_mesh(renderer, &scene.material_quad, scene.mesh_quad, model,
                               RENDER_LAYER_OPAQUE_3D);
        }

        // OBJ 模型
        {
            const f32  angle = spin * 0.8f;
            const vec3_t position = vec3_make(cosf(angle) * 3.6f, 1.2f, sinf(angle) * 3.6f);
            const mat4_t model = mat4_from_trs(position, quat_rotation_y(spin * 1.4f),
                                               vec3_splat(1.4f));
            renderer_push_mesh(renderer, &scene.material_obj, scene.mesh_obj, model,
                               RENDER_LAYER_OPAQUE_3D);
        }

        // 线框模式：把立方体与 OBJ 再画一遍（略放大避免 z-fighting）
        if (wireframe) {
            const mat4_t cube_model = mat4_from_trs(
                vec3_make(0.0f, 0.6f, 0.0f),
                quat_mul(quat_rotation_y(spin * 0.7f), quat_rotation_x(spin * 0.45f)),
                vec3_splat(1.01f));
            render_item_t item = render_item_default();
            item.pipeline = scene.pipeline_wireframe;
            item.mesh = scene.mesh_cube;
            item.layer = RENDER_LAYER_TRANSPARENT_3D;
            item.uniforms = object_uniforms_from_model(cube_model);
            item.uniforms.tint = vec4_make(0.35f, 1.0f, 0.55f, 1.0f);
            renderer_push_item(renderer, &item);
        }

        renderer_end_frame(renderer);

        frames_rendered++;

        if (clock.elapsed_seconds - last_title_update > 0.25) {
            last_title_update = clock.elapsed_seconds;

            render_stats_t stats;
            renderer_get_stats(renderer, &stats);

            snprintf(title, sizeof(title),
                     "MyEngine M2 | %.1f FPS | %.2f ms | 绘制 %u | 三角面 %u | 队列 %u | %s%s",
                     clock.fps, clock.delta_ms, stats.draw_calls, stats.triangles,
                     renderer_queue_count(renderer), show_perspective ? "透视" : "正交",
                     wireframe ? " | 线框" : "");
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
        LOG_INFO("渲染队列: 本帧 %u 项，历史峰值 %u 项", renderer_queue_count(renderer),
                 renderer_queue_peak_count(renderer));
        LOG_INFO("帧时间: 平均 %.3f ms | 最大 %.3f ms | 平滑 FPS %.1f", clock.delta_ms,
                 clock.max_delta_ms, clock.fps);
    }

    exit_code = 0;

cleanup:
    // 释放顺序：场景资源 -> 渲染器 -> 后端 -> 窗口
    if (scene_ready) {
        renderer_destroy_pipeline(renderer, scene.pipeline_lit);
        renderer_destroy_pipeline(renderer, scene.pipeline_lit_two_sided);
        renderer_destroy_pipeline(renderer, scene.pipeline_unlit);
        renderer_destroy_pipeline(renderer, scene.pipeline_wireframe);
        renderer_destroy_pipeline(renderer, scene.pipeline_lines);

        renderer_destroy_mesh(renderer, scene.mesh_plane);
        renderer_destroy_mesh(renderer, scene.mesh_cube);
        renderer_destroy_mesh(renderer, scene.mesh_quad);
        renderer_destroy_mesh(renderer, scene.mesh_grid);
        renderer_destroy_mesh(renderer, scene.mesh_obj);

        renderer_destroy_texture(renderer, scene.texture_checker);
        renderer_destroy_texture(renderer, scene.texture_gradient);
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
    printf("========== M2 验收 ==========\n");
    printf("  窗口 / 后端 / 渲染器 : %s\n", exit_code == 0 ? "创建成功" : "失败");
    printf("  渲染帧数             : %u\n", frames_rendered);
    printf("  内存泄漏             : %s\n", leaked ? "有" : "无");
    printf("  日志行数             : %llu\n", (unsigned long long)log_lines);
    printf("=============================\n");

    return (exit_code == 0 && !leaked) ? 0 : 1;
}
