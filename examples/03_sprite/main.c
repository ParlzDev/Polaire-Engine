// =============================================================================
//  examples/03_sprite/main.c —— M3 演示：2D 精灵批渲染
//
//  验证 M3 的验收标准：
//    * 1 万个精灵单帧 < 16ms
//    * draw call 是个位数（标题栏实时显示）
//    * 文本渲染（英文 + 中文）
//    * 2D 调试绘制
//    * 上层不直接调 GL
//
//  操作：
//      ESC      退出
//      1 / 2 / 3 / 4   切换精灵数量（1k / 10k / 50k / 容量上限）
//      F1       切换调试绘制
//      空格     暂停/继续动画
//      左键拖动 移动相机
//      滚轮     缩放
//
//  命令行：
//      --frames N   跑 N 帧后退出（CI 用）
//      --hidden     不显示窗口
//      --no-vsync   关闭垂直同步（测真实性能）
//      --count N    精灵数量
//      --no-text    不渲染文本（隔离测试：排除字体图集造成的额外 draw call）
// =============================================================================
#include <stdio.h>
#include <string.h>

#include "engine/engine.h"

// =============================================================================
//  程序化精灵图集
//
//  不依赖任何外部图片文件，示例开箱即跑。6 种形状画进一张 512x512 的图集，
//  所以 1 万个精灵全部来自同一张纹理 —— 这才是能合批的前提。
// =============================================================================
#define SHAPE_SIZE 64u
#define SHAPE_COUNT 6u

typedef f32 (*shape_sdf_fn)(vec2_t p);

// 统一成"只接受归一化坐标"的签名，这样才能放进函数指针数组。
// 参数不同的 SDF 就把它需要的常量写死在函数里。
static f32 sdf_box(vec2_t p)
{
    const vec2_t d = vec2_sub(vec2_abs(p), vec2_splat(0.75f));
    return vec2_length(vec2_max(d, vec2_zero())) + math_min(math_max(d.x, d.y), 0.0f);
}

static f32 sdf_circle(vec2_t p)
{
    return vec2_length(p) - 0.75f;
}

static f32 sdf_ring(vec2_t p)
{
    return fabsf(vec2_length(p) - 0.65f) - 0.12f;
}

static f32 sdf_diamond(vec2_t p)
{
    return (fabsf(p.x) + fabsf(p.y)) * 0.7071f - 0.8f;
}

static f32 sdf_triangle(vec2_t p)
{
    // 正三角形的三个半平面取交
    const f32 k = 0.866f;
    f32       d = p.y + 0.7f;
    d = math_max(d, -0.5f * p.x - k * p.y + 0.35f);
    d = math_max(d, 0.5f * p.x - k * p.y + 0.35f);
    return d;
}

static f32 sdf_star(vec2_t p)
{
    // 用极坐标做五角星：半径随角度在内外径之间来回
    const f32 angle = atan2f(p.y, p.x);
    const f32 radius = vec2_length(p);
    const f32 teeth = 5.0f;
    const f32 wave = cosf(angle * teeth * 2.0f) * 0.5f + 0.5f;
    const f32 outer = 0.85f;
    const f32 inner = 0.38f;
    const f32 target = math_lerp(inner, outer, wave);
    return radius - target;
}

static void generate_shape_pixels(u32 shape, u8 *out_rgba, u32 size)
{
    const shape_sdf_fn functions[SHAPE_COUNT] = {sdf_box,  sdf_circle, sdf_ring,
                                                 sdf_star, sdf_triangle, sdf_diamond};
    const shape_sdf_fn sdf = functions[shape % SHAPE_COUNT];

    const f32 half = (f32)size * 0.5f;

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            // 归一化到 [-1, 1]
            const vec2_t p = vec2_make(((f32)x + 0.5f - half) / half,
                                       ((f32)y + 0.5f - half) / half);

            const f32 d = sdf(p);

            // 1 像素宽的抗锯齿边缘
            const f32 coverage = math_saturate(0.5f - d * half);
            const u8  alpha = (u8)(coverage * 255.0f + 0.5f);

            u8 *pixel = &out_rgba[((usize)y * size + x) * 4u];
            // 每个形状给一点色相差异，方便肉眼区分
            pixel[0] = (u8)(200u + (shape * 11u) % 56u);
            pixel[1] = (u8)(200u + (shape * 23u) % 56u);
            pixel[2] = (u8)(200u + (shape * 37u) % 56u);
            pixel[3] = alpha;
        }
    }
}

// =============================================================================
//  命令行
// =============================================================================
typedef struct options_t {
    u32  width;
    u32  height;
    u32  frames;
    u32  sprite_count;
    bool hidden;
    bool vsync;
    bool text;
    bool unlimited; // 关掉帧率限制器，测真实性能
} options_t;

static void print_usage(void)
{
    printf("用法: 03_sprite [选项]\n");
    printf("  --frames N    跑 N 帧后退出（0 = 一直跑）\n");
    printf("  --count N     精灵数量（默认 10000）\n");
    printf("  --hidden      不显示窗口\n");
    printf("  --no-vsync    关闭垂直同步\n");
    printf("  --no-text     不渲染文本（隔离测试用）\n");
    printf("  --unlimited   关闭帧率限制器（测真实性能，CPU 会跑满）\n");
    printf("  --width/--height N\n");
    printf("\n运行时: ESC 退出 / 1-4 切换数量 / F1 调试绘制 / 空格 暂停 / 左键拖动 / 滚轮缩放\n");
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
    options.sprite_count = 10000;
    options.hidden = false;
    options.vsync = true;
    options.text = true;
    options.unlimited = false;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--frames") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.frames);
        } else if (strcmp(arg, "--count") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.sprite_count);
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.width);
        } else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.height);
        } else if (strcmp(arg, "--hidden") == 0) {
            options.hidden = true;
        } else if (strcmp(arg, "--no-vsync") == 0) {
            options.vsync = false;
        } else if (strcmp(arg, "--no-text") == 0) {
            options.text = false;
        } else if (strcmp(arg, "--unlimited") == 0) {
            options.unlimited = true;
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
//  伪随机（xorshift64*）
//
//  不用 rand()：它在不同 CRT 上序列不同，而且线程安全性依赖实现。
//  自己的 RNG 让"同一份种子产生同一份画面"成为可复现的事实 ——
//  性能对比时这一点很重要。
// =============================================================================
typedef struct rng_t {
    u64 state;
} rng_t;

static u32 rng_next_u32(rng_t *rng)
{
    rng->state ^= rng->state >> 12;
    rng->state ^= rng->state << 25;
    rng->state ^= rng->state >> 27;
    return (u32)((rng->state * 0x2545F4914F6CDD1DULL) >> 32);
}

static f32 rng_next_f32(rng_t *rng, f32 min_value, f32 max_value)
{
    const f32 t = (f32)rng_next_u32(rng) / 4294967296.0f;
    return min_value + (max_value - min_value) * t;
}

// =============================================================================
//  精灵实例
// =============================================================================
typedef struct sprite_instance_t {
    vec2_t base_position;
    f32    phase;
    f32    speed;
    f32    size;
    f32    spin;
    u32    shape;
    vec4_t color;
} sprite_instance_t;

// =============================================================================
//  main
// =============================================================================
int main(int argc, char **argv)
{
    const options_t options = parse_options(argc, argv);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    if (!log_init("03_sprite.log", LOG_LEVEL_DEBUG)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }

    LOG_INFO("MyEngine %s —— M3 精灵批渲染", MYENGINE_VERSION_STRING);
    LOG_INFO("目标精灵数: %u", options.sprite_count);

    if (!platform_time_init(true)) {
        log_shutdown();
        return 1;
    }

    int                exit_code = 1;
    platform_window_t *window = NULL;
    render_backend_t  *backend = NULL;
    renderer_t        *renderer = NULL;
    texture_atlas_t    atlas;
    bool               atlas_ready = false;
    texture_handle_t   atlas_texture = TEXTURE_HANDLE_INVALID;
    texture_handle_t   white_texture = TEXTURE_HANDLE_INVALID;
    font_t            *font = NULL;
    sprite_batch_t     batch;
    bool               batch_ready = false;
    debug_draw_t       debug;
    sprite_instance_t *instances = NULL;
    u32                active_count = options.sprite_count;
    u32                frames_rendered = 0;

    // ---- 窗口 ----
    platform_window_desc_t window_desc = platform_window_desc_default();
    window_desc.title = "MyEngine M3 —— 精灵批渲染";
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

    renderer = renderer_create(&tracker.base, backend);
    if (renderer == NULL) {
        goto cleanup;
    }

    // ---- 精灵图集 ----
    {
        texture_atlas_desc_t atlas_desc = texture_atlas_desc_default();
        atlas_desc.width = 512;
        atlas_desc.height = 512;
        atlas_desc.debug_name = "shape_atlas";
        if (!texture_atlas_init(&atlas, &tracker.base, &atlas_desc)) {
            goto cleanup;
        }
        atlas_ready = true;

        u8 *pixels = (u8 *)tracker.base.alloc(&tracker.base,
                                              (usize)SHAPE_SIZE * SHAPE_SIZE * 4u, 16);
        if (pixels == NULL) {
            goto cleanup;
        }

        for (u32 shape = 0; shape < SHAPE_COUNT; ++shape) {
            generate_shape_pixels(shape, pixels, SHAPE_SIZE);
            if (!texture_atlas_add(&atlas, pixels, SHAPE_SIZE, SHAPE_SIZE, NULL, NULL)) {
                LOG_ERROR("图集放不下形状 %u", shape);
                break;
            }
        }

        tracker.base.free(&tracker.base, pixels, (usize)SHAPE_SIZE * SHAPE_SIZE * 4u);

        if (!texture_atlas_upload(renderer, &atlas)) {
            goto cleanup;
        }
        atlas_texture = texture_atlas_texture(&atlas);

        LOG_INFO("图集就绪: %u 个形状，%ux%u，利用率 %.1f%%",
                 texture_atlas_region_count(&atlas), atlas.width, atlas.height,
                 (f64)texture_atlas_used_ratio(&atlas) * 100.0);
    }

    // 1x1 白色纹理（调试绘制用）
    white_texture = texture_create_solid(renderer, 255, 255, 255, 255);
    if (!texture_is_valid(renderer, white_texture)) {
        goto cleanup;
    }

    // ---- 字体 ----
    {
        // 优先用系统里的微软雅黑（支持中文），拿不到就退回内置 8x8 位图字体。
        const char *extra = "精灵批渲染测试帧绘制调用数量零泄漏内存兆秒性能验收";
        font = font_create_from_ttf_file(renderer, &tracker.base, "C:/Windows/Fonts/msyh.ttc",
                                         20.0f, extra, 0);
        if (font == NULL) {
            LOG_WARN("系统中找不到微软雅黑，退回内置 8x8 位图字体（不支持中文）");
            font = font_create_bitmap8x8(renderer, &tracker.base);
        }
        if (font == NULL) {
            LOG_ERROR("字体创建失败");
            goto cleanup;
        }
    }

    // ---- 批渲染器 ----
    {
        sprite_batch_desc_t batch_desc = sprite_batch_desc_default();
        // 容量要能装下"精灵 + 文字字形 + 调试图形"的总和
        batch_desc.max_sprites = options.sprite_count + 4096u;
        if (!sprite_batch_init(&batch, renderer, &tracker.base, &batch_desc)) {
            LOG_ERROR("批渲染器初始化失败");
            goto cleanup;
        }
        batch_ready = true;
    }

    debug_draw_init(&debug, &batch, white_texture);

    // ---- 精灵实例 ----
    instances = (sprite_instance_t *)tracker.base.alloc(
        &tracker.base, sizeof(sprite_instance_t) * active_count, 16);
    if (instances == NULL) {
        LOG_ERROR("精灵实例数组分配失败（%u 个）", active_count);
        goto cleanup;
    }

    {
        rng_t rng;
        rng.state = 0x9E3779B97F4A7C15ULL;

        for (u32 i = 0; i < active_count; ++i) {
            instances[i].base_position =
                vec2_make(rng_next_f32(&rng, -1400.0f, 1400.0f),
                          rng_next_f32(&rng, -900.0f, 900.0f));
            instances[i].phase = rng_next_f32(&rng, 0.0f, F32_TAU);
            instances[i].speed = rng_next_f32(&rng, 0.4f, 1.6f);
            instances[i].size = rng_next_f32(&rng, 8.0f, 28.0f);
            instances[i].spin = rng_next_f32(&rng, -1.2f, 1.2f);
            instances[i].shape = rng_next_u32(&rng) % SHAPE_COUNT;
            instances[i].color = vec4_make(rng_next_f32(&rng, 0.6f, 1.0f),
                                           rng_next_f32(&rng, 0.6f, 1.0f),
                                           rng_next_f32(&rng, 0.6f, 1.0f), 1.0f);
        }
    }

    // ---- 相机 ----
    camera_2d_t camera_2d = camera_2d_make(vec2_zero(), vec2_make((f32)options.width,
                                                                  (f32)options.height));

    // ---- 帧循环 ----
    frame_clock_t   clock;
    frame_limiter_t limiter;
    frame_clock_init(&clock);
    frame_limiter_init(&limiter, 60.0);
    // vsync 打开时帧率已被 SwapBuffers 锁住，再叠一层限制器只会让帧时间更不稳。
    // --unlimited 则两层都关掉，用来测真实性能。
    if (options.vsync || options.unlimited) {
        limiter.enabled = false;
    }
    frame_clock_reset(&clock);

    renderer_set_clear(renderer, true, vec4_make(0.06f, 0.07f, 0.10f, 1.0f), true, 1.0f);

    text_style_t hud_style = text_style_default(font);
    hud_style.color = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);

    char   title[256];
    f64    last_title_update = 0.0;
    f32    elapsed = 0.0f;
    bool   paused = false;
    bool   show_debug = true;
    bool   dragging = false;
    f32    worst_frame_ms = 0.0f;

    while (true) {
        const platform_time_point_t frame_start = platform_time_now();

        if (!platform_window_poll_events(window)) {
            break;
        }

        const input_state_t *input = platform_window_input(window);
        if (input_key_pressed(input, KEY_ESCAPE)) {
            break;
        }
        if (input_key_pressed(input, KEY_SPACE)) {
            paused = !paused;
            LOG_INFO("动画: %s", paused ? "暂停" : "继续");
        }
        if (input_key_pressed(input, KEY_F1)) {
            show_debug = !show_debug;
        }
        if (input_key_pressed(input, KEY_1)) {
            active_count = 1000;
        }
        if (input_key_pressed(input, KEY_2)) {
            active_count = 10000;
        }
        if (input_key_pressed(input, KEY_3)) {
            active_count = 50000;
        }
        if (input_key_pressed(input, KEY_4)) {
            active_count = options.sprite_count;
        }

        if (input_mouse_pressed(input, MOUSE_BUTTON_LEFT)) {
            dragging = true;
        }
        if (input_mouse_released(input, MOUSE_BUTTON_LEFT)) {
            dragging = false;
        }
        if (dragging) {
            camera_2d.position.x -= input->mouse_dx / camera_2d.zoom;
            camera_2d.position.y += input->mouse_dy / camera_2d.zoom;
        }
        if (input->scroll_dy != 0.0f) {
            camera_2d.zoom = math_clamp(camera_2d.zoom * (1.0f + input->scroll_dy * 0.12f), 0.1f,
                                        6.0f);
        }

        const f64 delta = frame_clock_tick(&clock);
        if (!paused) {
            elapsed += (f32)delta;
        }

        u32 width = 0;
        u32 height = 0;
        platform_window_get_size(window, &width, &height);
        if (width == 0 || height == 0) {
            frame_limiter_wait(&limiter, frame_start);
            continue;
        }
        camera_2d.viewport_size = vec2_make((f32)width, (f32)height);

        // ---- 一帧 ----
        frame_desc_t frame_desc;
        memset(&frame_desc, 0, sizeof(frame_desc));
        frame_desc.target_width = width;
        frame_desc.target_height = height;
        frame_desc.delta_seconds = (f32)delta;
        frame_desc.elapsed_seconds = clock.elapsed_seconds;

        const camera_t camera = camera_2d_to_camera(&camera_2d);
        renderer_begin_frame(renderer, &camera, &frame_desc);

        sprite_batch_begin(&batch);
        sprite_batch_set_layer(&batch, RENDER_LAYER_TRANSPARENT_2D);
        debug_draw_begin(&debug);

        // ---- 精灵 ----
        const u32 shape_count = texture_atlas_region_count(&atlas);
        for (u32 i = 0; i < active_count; ++i) {
            const sprite_instance_t *instance = &instances[i];

            const f32 wobble = sinf(elapsed * instance->speed + instance->phase);
            sprite_t  sprite = sprite_default();
            sprite.position = vec2_make(instance->base_position.x + wobble * 40.0f,
                                        instance->base_position.y + cosf(elapsed * instance->speed * 0.7f
                                                                         + instance->phase) * 30.0f);
            sprite.size = vec2_splat(instance->size * (1.0f + wobble * 0.15f));
            sprite.rotation = elapsed * instance->spin;
            sprite.color = instance->color;
            sprite.uv_min = texture_atlas_region(&atlas, instance->shape % shape_count)->uv_min;
            sprite.uv_max = texture_atlas_region(&atlas, instance->shape % shape_count)->uv_max;

            sprite_batch_draw(&batch, &sprite, atlas_texture);
        }

        // ---- 调试绘制 ----
        if (show_debug) {
            const vec2_t view_min = vec2_make(camera_2d.position.x - camera_2d.viewport_size.x *
                                                             0.5f / camera_2d.zoom,
                                              camera_2d.position.y - camera_2d.viewport_size.y *
                                                             0.5f / camera_2d.zoom);
            const vec2_t view_max = vec2_make(camera_2d.position.x + camera_2d.viewport_size.x *
                                                             0.5f / camera_2d.zoom,
                                              camera_2d.position.y + camera_2d.viewport_size.y *
                                                             0.5f / camera_2d.zoom);

            debug_draw_grid(&debug, vec2_make(floorf(view_min.x / 100.0f) * 100.0f,
                                              floorf(view_min.y / 100.0f) * 100.0f),
                            vec2_make(ceilf(view_max.x / 100.0f) * 100.0f,
                                      ceilf(view_max.y / 100.0f) * 100.0f),
                            100.0f, vec4_make(0.25f, 0.30f, 0.40f, 0.6f), 1.0f);

            // 屏幕中心画个准星，并显示鼠标位置
            const vec2_t mouse_world =
                camera_2d_screen_to_world(&camera_2d, vec2_make(input->mouse_x, input->mouse_y));
            debug_draw_cross(&debug, mouse_world, 12.0f / camera_2d.zoom,
                             vec4_make(1.0f, 0.4f, 0.3f, 0.9f), 2.0f / camera_2d.zoom);
            debug_draw_circle(&debug, mouse_world, 40.0f, vec4_make(1.0f, 0.6f, 0.2f, 0.5f), false,
                              32, 1.5f / camera_2d.zoom);
        }

        // ---- HUD ----
        if (options.text) {
            const f32 top = (f32)height * 0.5f - 24.0f;
            const f32 left = -(f32)width * 0.5f + 16.0f;

            text_style_t style = hud_style;
            style.align = TEXT_ALIGN_LEFT;
            style.valign = TEXT_VALIGN_TOP;

            text_draw_fmt(&batch, &style, vec2_make(left, top),
                          "精灵 %u / 绘制调用 %u / 帧 %.2f ms / FPS %.0f", active_count,
                          sprite_batch_batch_count(&batch), clock.delta_ms, clock.fps);

            style.color = vec4_make(0.7f, 0.8f, 1.0f, 1.0f);
            text_draw_fmt(&batch, &style, vec2_make(left, top - 26.0f),
                          "批渲染容量 %u，峰值 %u，丢弃 %u", batch.sprite_capacity,
                          sprite_batch_peak_sprite_count(&batch),
                          sprite_batch_dropped_sprites(&batch));

            style.color = vec4_make(1.0f, 0.85f, 0.5f, 1.0f);
            text_draw_fmt(&batch, &style, vec2_make(left, top - 52.0f),
                          "1-4 切换数量 / F1 调试 / 空格 %s / 拖动平移 / 滚轮缩放",
                          paused ? "继续" : "暂停");
        }

        sprite_batch_end(&batch);
        renderer_end_frame(renderer);

        frames_rendered++;
        if (clock.raw_delta_ms > (f64)worst_frame_ms && frames_rendered > 5) {
            worst_frame_ms = (f32)clock.raw_delta_ms;
        }

        if (clock.elapsed_seconds - last_title_update > 0.25) {
            last_title_update = clock.elapsed_seconds;

            render_stats_t stats;
            renderer_get_stats(renderer, &stats);

            snprintf(title, sizeof(title),
                     "MyEngine M3 | 精灵 %u | 绘制调用 %u | %.2f ms | %.0f FPS", active_count,
                     stats.draw_calls, clock.delta_ms, clock.fps);
            platform_window_set_title(window, title);
        }

        if (options.frames > 0 && frames_rendered >= options.frames) {
            break;
        }

        frame_limiter_wait(&limiter, frame_start);
    }

    // ---- 总结 ----
    {
        render_stats_t stats;
        renderer_get_stats(renderer, &stats);

        LOG_INFO("共渲染 %u 帧，总时长 %.3f 秒", frames_rendered, clock.elapsed_seconds);
        LOG_INFO("精灵数 %u | 绘制调用 %u | 三角面 %u | 顶点 %u", active_count, stats.draw_calls,
                 stats.triangles, stats.vertices);
        LOG_INFO("帧时间: 平均 %.3f ms | 最差 %.3f ms | 平滑 FPS %.1f", clock.delta_ms,
                 (f64)worst_frame_ms, clock.fps);
    }

    exit_code = 0;

cleanup:
    if (instances != NULL) {
        tracker.base.free(&tracker.base, instances, sizeof(sprite_instance_t) * active_count);
    }
    if (font != NULL) {
        font_destroy(font);
    }
    if (batch_ready) {
        sprite_batch_shutdown(&batch);
    }
    if (renderer != NULL) {
        if (texture_is_valid(renderer, white_texture)) {
            renderer_destroy_texture(renderer, white_texture);
        }
        renderer_destroy(renderer);
    }
    if (atlas_ready) {
        texture_atlas_shutdown(&atlas);
    }
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
    printf("========== M3 精灵压测 ==========\n");
    printf("  精灵数量   : %u\n", active_count);
    printf("  渲染帧数   : %u\n", frames_rendered);
    printf("  内存泄漏   : %s\n", leaked ? "有" : "无");
    printf("  日志行数   : %llu\n", (unsigned long long)log_lines);
    printf("=================================\n");

    return (exit_code == 0 && !leaked) ? 0 : 1;
}
