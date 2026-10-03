// =============================================================================
//  examples/04_breakout/main.c —— M3 验收：打砖块
//
//  这是 M3 的"验证游戏"：它把 2D 渲染管线、输入、计时、文本、
//  调试绘制全部串起来跑一遍。
//
//  操作：
//      鼠标 / A D      移动挡板
//      空格 / 左键     发球
//      P               暂停
//      R               重开
//      F1              调试绘制（碰撞盒、球速向量）
//      ESC             退出
//
//  命令行：
//      --frames N      跑 N 帧后退出（CI 用）
//      --hidden        不显示窗口
//      --no-vsync      关闭垂直同步
//      --autoplay      自动玩（无人值守回归用：挡板自动跟球）
// =============================================================================
#include <stdio.h>
#include <string.h>

#include "engine/engine.h"

// =============================================================================
//  玩法常量
// =============================================================================
#define PLAYFIELD_WIDTH 960.0f
#define PLAYFIELD_HEIGHT 640.0f
#define PLAYFIELD_LEFT (-PLAYFIELD_WIDTH * 0.5f)
#define PLAYFIELD_RIGHT (PLAYFIELD_WIDTH * 0.5f)
#define PLAYFIELD_BOTTOM (-PLAYFIELD_HEIGHT * 0.5f)
#define PLAYFIELD_TOP (PLAYFIELD_HEIGHT * 0.5f)

#define PADDLE_WIDTH 140.0f
#define PADDLE_HEIGHT 18.0f
#define PADDLE_Y (PLAYFIELD_BOTTOM + 48.0f)
#define PADDLE_SPEED 900.0f

#define BALL_RADIUS 9.0f
#define BALL_SPEED 520.0f

#define BRICK_COLUMNS 11
#define BRICK_ROWS 6
#define BRICK_WIDTH 76.0f
#define BRICK_HEIGHT 26.0f
#define BRICK_GAP_X 6.0f
#define BRICK_GAP_Y 8.0f
#define BRICK_TOP (PLAYFIELD_TOP - 60.0f)

#define MAX_LIVES 3

// 物理固定步长。用固定步长而不是可变 dt：
// 可变 dt 会让"同一局游戏在不同帧率下结果不同"，碰撞解算会漂移，
// 而且高速球在小 dt 和大 dt 下的行为完全不一样。
#define FIXED_DT (1.0f / 120.0f)
#define MAX_STEPS_PER_FRAME 8

// =============================================================================
//  图集（程序化生成，不依赖外部资源）
// =============================================================================
#define ATLAS_SHAPE_SIZE 64u
#define SHAPE_BALL 0u
#define SHAPE_PADDLE 1u
#define SHAPE_BRICK 2u
#define SHAPE_SPARK 3u

static f32 sdf_round_box(vec2_t p, vec2_t half, f32 radius)
{
    const vec2_t d = vec2_sub(vec2_abs(p), vec2_sub(half, vec2_splat(radius)));
    return vec2_length(vec2_max(d, vec2_zero())) + math_min(math_max(d.x, d.y), 0.0f) - radius;
}

static void generate_shape(u32 shape, u8 *out_rgba, u32 size)
{
    const f32 half = (f32)size * 0.5f;

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const vec2_t p = vec2_make(((f32)x + 0.5f - half) / half,
                                       ((f32)y + 0.5f - half) / half);

            f32 d = 1.0f;
            switch (shape) {
            case SHAPE_BALL:
                d = vec2_length(p) - 0.92f;
                break;
            case SHAPE_PADDLE:
                d = sdf_round_box(p, vec2_make(0.98f, 0.72f), 0.45f);
                break;
            case SHAPE_BRICK:
                d = sdf_round_box(p, vec2_make(0.97f, 0.85f), 0.22f);
                break;
            case SHAPE_SPARK:
            default: {
                const f32 angle = atan2f(p.y, p.x);
                const f32 wave = cosf(angle * 6.0f) * 0.5f + 0.5f;
                d = vec2_length(p) - math_lerp(0.35f, 0.95f, wave);
                break;
            }
            }

            const f32 coverage = math_saturate(0.5f - d * half);
            u8 *pixel = &out_rgba[((usize)y * size + x) * 4u];
            // 白色 + alpha：颜色完全由精灵的 tint 决定，
            // 这样一张图集就能画出所有颜色的砖块。
            pixel[0] = 255;
            pixel[1] = 255;
            pixel[2] = 255;
            pixel[3] = (u8)(coverage * 255.0f + 0.5f);
        }
    }
}

// =============================================================================
//  游戏状态
// =============================================================================
typedef enum game_state_t {
    GAME_STATE_READY = 0, // 球粘在挡板上，等发球
    GAME_STATE_PLAYING,
    GAME_STATE_PAUSED,
    GAME_STATE_GAME_OVER,
    GAME_STATE_WIN
} game_state_t;

typedef struct brick_t {
    vec2_t position;
    u32    row;
    bool   alive;
} brick_t;

typedef struct ball_t {
    vec2_t position;
    vec2_t velocity;
    f32    radius;
} ball_t;

typedef struct game_t {
    game_state_t state;

    vec2_t  paddle_position;
    f32     paddle_width;

    ball_t ball;

    brick_t bricks[BRICK_COLUMNS * BRICK_ROWS];
    u32     bricks_alive;

    u32 score;
    u32 best_score;
    u32 lives;
    u32 level;

    f32 shake_time; // 命中砖块时的轻微震动（纯手感）
} game_t;

static void game_reset_level(game_t *game)
{
    game->bricks_alive = 0;

    for (u32 row = 0; row < BRICK_ROWS; ++row) {
        for (u32 col = 0; col < BRICK_COLUMNS; ++col) {
            const f32 total_width = (f32)BRICK_COLUMNS * (BRICK_WIDTH + BRICK_GAP_X) - BRICK_GAP_X;
            const f32 start_x = -total_width * 0.5f + BRICK_WIDTH * 0.5f;

            brick_t *brick = &game->bricks[row * BRICK_COLUMNS + col];
            brick->position = vec2_make(start_x + (f32)col * (BRICK_WIDTH + BRICK_GAP_X),
                                        BRICK_TOP - (f32)row * (BRICK_HEIGHT + BRICK_GAP_Y));
            brick->row = row;
            // 最上面两行的砖块在更高的关卡里才出现
            brick->alive = (row >= 2u - (game->level % 3u)) ? true : (game->level > 1u);
            if (brick->alive) {
                game->bricks_alive++;
            }
        }
    }

    game->paddle_position = vec2_make(0.0f, PADDLE_Y);
    game->paddle_width = PADDLE_WIDTH;

    game->ball.position = vec2_make(0.0f, PADDLE_Y + PADDLE_HEIGHT * 0.5f + BALL_RADIUS + 2.0f);
    game->ball.velocity = vec2_zero();
    game->ball.radius = BALL_RADIUS;

    game->state = GAME_STATE_READY;
}

static void game_restart(game_t *game)
{
    game->score = 0;
    game->lives = MAX_LIVES;
    game->level = 1;
    game->shake_time = 0.0f;
    game_reset_level(game);
}

static void game_launch_ball(game_t *game)
{
    if (game->state != GAME_STATE_READY) {
        return;
    }

    // 稍微随机的初始角度，避免每局完全一样
    const f32 angle = math_radians(60.0f) + (f32)(game->level % 5u) * math_radians(6.0f);
    game->ball.velocity = vec2_make(cosf(angle) * BALL_SPEED, sinf(angle) * BALL_SPEED);
    game->state = GAME_STATE_PLAYING;
}

// ---------------------------------------------------------------- 碰撞
//
// 圆 vs AABB。返回是否相交，并给出碰撞法线与穿透深度。
// 用"最近点"而不是"逐轴分离"：逐轴分离在角上会给出错误法线，
// 表现为球撞到砖块角上时朝奇怪的方向弹开。
static bool circle_vs_aabb(vec2_t center, f32 radius, vec2_t box_min, vec2_t box_max,
                           vec2_t *out_normal, f32 *out_depth)
{
    const vec2_t closest = vec2_make(math_clamp(center.x, box_min.x, box_max.x),
                                     math_clamp(center.y, box_min.y, box_max.y));
    const vec2_t delta = vec2_sub(center, closest);
    const f32    distance_sq = vec2_length_sq(delta);

    if (distance_sq > radius * radius) {
        return false;
    }

    if (distance_sq > 1.0e-8f) {
        const f32 distance = sqrtf(distance_sq);
        if (out_normal != NULL) {
            *out_normal = vec2_scale(delta, 1.0f / distance);
        }
        if (out_depth != NULL) {
            *out_depth = radius - distance;
        }
        return true;
    }

    // 圆心在盒子内部：退化成"找最近的边推出去"。
    // 这种情况在高速球上会真实发生，不处理会让球卡在砖块里。
    const f32 dx_left = center.x - box_min.x;
    const f32 dx_right = box_max.x - center.x;
    const f32 dy_bottom = center.y - box_min.y;
    const f32 dy_top = box_max.y - center.y;

    f32    best = dx_left;
    vec2_t normal = vec2_make(-1.0f, 0.0f);
    if (dx_right < best) {
        best = dx_right;
        normal = vec2_make(1.0f, 0.0f);
    }
    if (dy_bottom < best) {
        best = dy_bottom;
        normal = vec2_make(0.0f, -1.0f);
    }
    if (dy_top < best) {
        best = dy_top;
        normal = vec2_make(0.0f, 1.0f);
    }

    if (out_normal != NULL) {
        *out_normal = normal;
    }
    if (out_depth != NULL) {
        *out_depth = radius + best;
    }
    return true;
}

static void game_ball_step(game_t *game, f32 dt)
{
    ball_t *ball = &game->ball;

    ball->position = vec2_mul_add(ball->position, ball->velocity, dt);

    // 左右墙
    if (ball->position.x - ball->radius < PLAYFIELD_LEFT) {
        ball->position.x = PLAYFIELD_LEFT + ball->radius;
        ball->velocity.x = fabsf(ball->velocity.x);
    } else if (ball->position.x + ball->radius > PLAYFIELD_RIGHT) {
        ball->position.x = PLAYFIELD_RIGHT - ball->radius;
        ball->velocity.x = -fabsf(ball->velocity.x);
    }

    // 天花板
    if (ball->position.y + ball->radius > PLAYFIELD_TOP) {
        ball->position.y = PLAYFIELD_TOP - ball->radius;
        ball->velocity.y = -fabsf(ball->velocity.y);
    }

    // 挡板：用"命中位置决定反弹角"的经典做法 ——
    // 直接镜面反射会让球路越打越平，玩起来很闷。
    {
        const vec2_t paddle_min = vec2_make(game->paddle_position.x - game->paddle_width * 0.5f,
                                            game->paddle_position.y - PADDLE_HEIGHT * 0.5f);
        const vec2_t paddle_max = vec2_make(game->paddle_position.x + game->paddle_width * 0.5f,
                                            game->paddle_position.y + PADDLE_HEIGHT * 0.5f);

        vec2_t normal;
        f32    depth;
        if (ball->velocity.y < 0.0f &&
            circle_vs_aabb(ball->position, ball->radius, paddle_min, paddle_max, &normal, &depth)) {
            ball->position = vec2_mul_add(ball->position, normal, depth + 0.01f);

            const f32 offset =
                math_clamp((ball->position.x - game->paddle_position.x) /
                               (game->paddle_width * 0.5f),
                           -1.0f, 1.0f);
            // 最大偏折 60 度
            const f32 angle = offset * math_radians(60.0f);
            const f32 speed = math_max(vec2_length(ball->velocity), BALL_SPEED);

            ball->velocity = vec2_make(sinf(angle) * speed, cosf(angle) * speed);
            ball->velocity.y = fabsf(ball->velocity.y);
        }
    }

    // 砖块
    for (u32 i = 0; i < BRICK_COLUMNS * BRICK_ROWS; ++i) {
        brick_t *brick = &game->bricks[i];
        if (!brick->alive) {
            continue;
        }

        const vec2_t box_min = vec2_make(brick->position.x - BRICK_WIDTH * 0.5f,
                                         brick->position.y - BRICK_HEIGHT * 0.5f);
        const vec2_t box_max = vec2_make(brick->position.x + BRICK_WIDTH * 0.5f,
                                         brick->position.y + BRICK_HEIGHT * 0.5f);

        vec2_t normal;
        f32    depth;
        if (!circle_vs_aabb(ball->position, ball->radius, box_min, box_max, &normal, &depth)) {
            continue;
        }

        // 推出穿透 + 按法线反射
        ball->position = vec2_mul_add(ball->position, normal, depth + 0.01f);
        const f32 dot = vec2_dot(ball->velocity, normal);
        ball->velocity = vec2_sub(ball->velocity, vec2_scale(normal, 2.0f * dot));

        brick->alive = false;
        game->bricks_alive--;
        game->score += (BRICK_ROWS - brick->row) * 10u;
        game->shake_time = 0.08f;

        // 每步只处理一次碰撞：一帧内连撞两块砖是可能的，
        // 但继续处理会让球在两块砖之间来回弹（法线互相抵消）。
        break;
    }

    // 掉出底部
    if (ball->position.y + ball->radius < PLAYFIELD_BOTTOM) {
        game->lives--;
        if (game->lives == 0) {
            if (game->score > game->best_score) {
                game->best_score = game->score;
            }
            game->state = GAME_STATE_GAME_OVER;
        } else {
            game_reset_level(game);
            game->state = GAME_STATE_READY;
        }
    }

    if (game->bricks_alive == 0) {
        if (game->score > game->best_score) {
            game->best_score = game->score;
        }
        game->state = GAME_STATE_WIN;
    }
}

// ---------------------------------------------------------------- 更新
static void game_update(game_t *game, f32 dt, f32 paddle_input, bool autoplay)
{
    if (game->shake_time > 0.0f) {
        game->shake_time = math_max(0.0f, game->shake_time - dt);
    }

    if (game->state == GAME_STATE_GAME_OVER || game->state == GAME_STATE_WIN) {
        return;
    }

    // 挡板
    {
        f32 target_x = game->paddle_position.x;

        if (autoplay) {
            // 自动玩：挡板缓慢追踪球的 X
            target_x = math_lerp(game->paddle_position.x, game->ball.position.x, 6.0f * dt);
        } else if (paddle_input != 0.0f) {
            target_x += paddle_input * PADDLE_SPEED * dt;
        }

        const f32 limit = PLAYFIELD_WIDTH * 0.5f - game->paddle_width * 0.5f;
        game->paddle_position.x = math_clamp(target_x, -limit, limit);
    }

    if (game->state == GAME_STATE_READY) {
        // 球粘在挡板上
        game->ball.position = vec2_make(game->paddle_position.x,
                                        PADDLE_Y + PADDLE_HEIGHT * 0.5f + BALL_RADIUS + 2.0f);
        return;
    }

    if (game->state != GAME_STATE_PLAYING) {
        return;
    }

    // 子步进：把这一帧的位移拆成若干小步，每步都做碰撞检测。
    //
    // 不做的话，速度 520 像素/秒的球在 1/120 秒里走 4.3 像素 —— 还没问题；
    // 但如果 dt 因为掉帧变成 1/30，一帧就是 17 像素，球会直接穿过
    // 26 像素高的砖块中间的缝。子步进把"穿墙"彻底消除。
    const f32 speed = vec2_length(game->ball.velocity);
    const f32 max_step = game->ball.radius * 0.6f;
    const f32 total_distance = speed * dt;

    u32 steps = (u32)(total_distance / max_step) + 1u;
    if (steps > 16u) {
        steps = 16u;
    }

    const f32 sub_dt = dt / (f32)steps;
    for (u32 i = 0; i < steps; ++i) {
        game_ball_step(game, sub_dt);
        if (game->state != GAME_STATE_PLAYING) {
            break;
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
    bool hidden;
    bool vsync;
    bool autoplay;
} options_t;

static void print_usage(void)
{
    printf("用法: 04_breakout [选项]\n");
    printf("  --frames N   跑 N 帧后退出（0 = 一直跑）\n");
    printf("  --hidden     不显示窗口\n");
    printf("  --no-vsync   关闭垂直同步\n");
    printf("  --autoplay   自动玩（无人值守回归用）\n");
    printf("\n运行时: 鼠标/AD 移动挡板 / 空格发球 / P 暂停 / R 重开 / F1 调试 / ESC 退出\n");
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
    options.vsync = true;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--frames") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.frames);
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.width);
        } else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            parse_u32(argv[++i], &options.height);
        } else if (strcmp(arg, "--hidden") == 0) {
            options.hidden = true;
        } else if (strcmp(arg, "--no-vsync") == 0) {
            options.vsync = false;
        } else if (strcmp(arg, "--autoplay") == 0) {
            options.autoplay = true;
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
//  砖块配色
// =============================================================================
static vec4_t brick_color(u32 row)
{
    static const vec4_t colors[BRICK_ROWS] = {
        {1.00f, 0.35f, 0.35f, 1.0f}, // 红
        {1.00f, 0.62f, 0.30f, 1.0f}, // 橙
        {1.00f, 0.88f, 0.32f, 1.0f}, // 黄
        {0.45f, 0.90f, 0.45f, 1.0f}, // 绿
        {0.38f, 0.68f, 1.00f, 1.0f}, // 蓝
        {0.72f, 0.48f, 1.00f, 1.0f}, // 紫
    };
    return colors[row % BRICK_ROWS];
}

// =============================================================================
//  main
// =============================================================================
int main(int argc, char **argv)
{
    const options_t options = parse_options(argc, argv);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    if (!log_init("04_breakout.log", LOG_LEVEL_DEBUG)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }

    LOG_INFO("Polaire Engine %s —— M3 打砖块", POLAIRE_VERSION_STRING);

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
    game_t             game;
    u32                frames_rendered = 0;

    platform_window_desc_t window_desc = platform_window_desc_default();
    window_desc.title = "Polaire Engine M3 —— 打砖块";
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
        goto cleanup;
    }

    render_backend_desc_t backend_desc;
    memset(&backend_desc, 0, sizeof(backend_desc));
    backend_desc.window = window;
    backend_desc.msaa_samples = 4;
    backend_desc.vsync = options.vsync;
    backend_desc.enable_debug_output = true;
    backend_desc.debug_name = "breakout";

    if (!render_backend_init(backend, &backend_desc)) {
        goto cleanup;
    }

    renderer = renderer_create(&tracker.base, backend);
    if (renderer == NULL) {
        goto cleanup;
    }

    // ---- 图集 ----
    {
        texture_atlas_desc_t atlas_desc = texture_atlas_desc_default();
        atlas_desc.width = 256;
        atlas_desc.height = 256;
        atlas_desc.debug_name = "breakout_atlas";
        if (!texture_atlas_init(&atlas, &tracker.base, &atlas_desc)) {
            goto cleanup;
        }
        atlas_ready = true;

        u8 *pixels = (u8 *)tracker.base.alloc(&tracker.base,
                                              (usize)ATLAS_SHAPE_SIZE * ATLAS_SHAPE_SIZE * 4u, 16);
        if (pixels == NULL) {
            goto cleanup;
        }

        for (u32 shape = 0; shape < 4; ++shape) {
            generate_shape(shape, pixels, ATLAS_SHAPE_SIZE);
            texture_atlas_add(&atlas, pixels, ATLAS_SHAPE_SIZE, ATLAS_SHAPE_SIZE, NULL, NULL);
        }

        tracker.base.free(&tracker.base, pixels,
                          (usize)ATLAS_SHAPE_SIZE * ATLAS_SHAPE_SIZE * 4u);

        if (!texture_atlas_upload(renderer, &atlas)) {
            goto cleanup;
        }
        atlas_texture = texture_atlas_texture(&atlas);
    }

    white_texture = texture_create_solid(renderer, 255, 255, 255, 255);
    if (!texture_is_valid(renderer, white_texture)) {
        goto cleanup;
    }

    // ---- 字体 ----
    {
        const char *extra =
            "分数生命关卡暂停游戏结束胜利按空格发球重开调试继续最高"
            "打砖块演示精灵批渲染测试";
        font = font_create_from_ttf_file(renderer, &tracker.base, "C:/Windows/Fonts/msyh.ttc",
                                         22.0f, extra, 0);
        if (font == NULL) {
            LOG_WARN("找不到微软雅黑，退回内置 8x8 位图字体（中文会显示为空白）");
            font = font_create_bitmap8x8(renderer, &tracker.base);
        }
        if (font == NULL) {
            goto cleanup;
        }
    }

    // ---- 批渲染器 ----
    {
        sprite_batch_desc_t batch_desc = sprite_batch_desc_default();
        batch_desc.max_sprites = 1024; // 砖块 66 + 球 + 挡板 + 文字 + 调试图形，绰绰有余
        if (!sprite_batch_init(&batch, renderer, &tracker.base, &batch_desc)) {
            goto cleanup;
        }
        batch_ready = true;
    }

    debug_draw_init(&debug, &batch, white_texture);

    // ---- 游戏 ----
    memset(&game, 0, sizeof(game));
    game_restart(&game);

    camera_2d_t camera_2d = camera_2d_make(vec2_zero(), vec2_make((f32)options.width,
                                                                  (f32)options.height));

    frame_clock_t   clock;
    frame_limiter_t limiter;
    frame_clock_init(&clock);
    frame_limiter_init(&limiter, 60.0);
    if (options.vsync) {
        limiter.enabled = false;
    }
    frame_clock_reset(&clock);

    renderer_set_clear(renderer, true, vec4_make(0.04f, 0.05f, 0.08f, 1.0f), true, 1.0f);

    text_style_t ui_style = text_style_default(font);
    text_style_t center_style = text_style_default(font);
    center_style.align = TEXT_ALIGN_CENTER;
    center_style.valign = TEXT_VALIGN_MIDDLE;

    f64  accumulator = 0.0;
    char title[256];
    f64  last_title_update = 0.0;
    bool show_debug = false;

    while (true) {
        const platform_time_point_t frame_start = platform_time_now();

        if (!platform_window_poll_events(window)) {
            break;
        }

        const input_state_t *input = platform_window_input(window);
        if (input_key_pressed(input, KEY_ESCAPE)) {
            break;
        }
        if (input_key_pressed(input, KEY_R)) {
            game_restart(&game);
            LOG_INFO("重开");
        }
        if (input_key_pressed(input, KEY_P)) {
            if (game.state == GAME_STATE_PLAYING || game.state == GAME_STATE_READY) {
                game.state = GAME_STATE_PAUSED;
            } else if (game.state == GAME_STATE_PAUSED) {
                game.state = GAME_STATE_PLAYING;
            }
        }
        if (input_key_pressed(input, KEY_F1)) {
            show_debug = !show_debug;
        }
        if (input_key_pressed(input, KEY_SPACE) || input_mouse_pressed(input, MOUSE_BUTTON_LEFT)) {
            if (game.state == GAME_STATE_READY) {
                game_launch_ball(&game);
            } else if (game.state == GAME_STATE_GAME_OVER || game.state == GAME_STATE_WIN) {
                game_restart(&game);
            }
        }

        const f64 delta = frame_clock_tick(&clock);

        u32 width = 0;
        u32 height = 0;
        platform_window_get_size(window, &width, &height);
        if (width == 0 || height == 0) {
            frame_limiter_wait(&limiter, frame_start);
            continue;
        }
        camera_2d.viewport_size = vec2_make((f32)width, (f32)height);

        // ---- 固定步长物理 ----
        {
            // 挡板输入：鼠标优先，其次 A/D
            f32 paddle_input = 0.0f;
            if (input_key_down(input, KEY_A) || input_key_down(input, KEY_LEFT)) {
                paddle_input -= 1.0f;
            }
            if (input_key_down(input, KEY_D) || input_key_down(input, KEY_RIGHT)) {
                paddle_input += 1.0f;
            }

            if (paddle_input == 0.0f && !options.autoplay) {
                // 鼠标绝对定位（比键盘更跟手，打砖块的标配）
                const vec2_t mouse_world =
                    camera_2d_screen_to_world(&camera_2d, vec2_make(input->mouse_x, input->mouse_y));
                const f32 limit = PLAYFIELD_WIDTH * 0.5f - game.paddle_width * 0.5f;
                game.paddle_position.x = math_clamp(mouse_world.x, -limit, limit);
            }

            accumulator += delta;
            // 上限保护：如果某一帧特别长（例如窗口拖动、断点），
            // 不做上限会进入"越补越慢"的死亡螺旋。
            if (accumulator > (f64)FIXED_DT * (f64)MAX_STEPS_PER_FRAME) {
                accumulator = (f64)FIXED_DT * (f64)MAX_STEPS_PER_FRAME;
            }

            // 自动玩模式下自动发球 + 自动重开，否则无人值守跑出来的
            // 永远是"球粘在挡板上"的画面，等于没测到任何东西。
            if (options.autoplay) {
                if (game.state == GAME_STATE_READY) {
                    game_launch_ball(&game);
                } else if (game.state == GAME_STATE_GAME_OVER || game.state == GAME_STATE_WIN) {
                    game_restart(&game);
                }
            }

            while (accumulator >= (f64)FIXED_DT) {
                game_update(&game, FIXED_DT, paddle_input, options.autoplay);
                accumulator -= (f64)FIXED_DT;
            }
        }

        // ---- 渲染 ----
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

        // 命中砖块时的轻微震动：纯手感，但少了会觉得打击感很空
        const vec2_t shake =
            (game.shake_time > 0.0f)
                ? vec2_make(sinf(game.shake_time * 200.0f) * 3.0f,
                            cosf(game.shake_time * 180.0f) * 2.0f)
                : vec2_zero();

        // 场地边框
        {
            const atlas_region_t *region = texture_atlas_region(&atlas, SHAPE_BRICK);
            const f32 thickness = 4.0f;
            const vec4_t wall_color = vec4_make(0.30f, 0.36f, 0.48f, 1.0f);

            sprite_batch_draw_region(&batch,
                                     vec2_make(0.0f, PLAYFIELD_BOTTOM - thickness * 0.5f),
                                     vec2_make(PLAYFIELD_WIDTH, thickness), 0.0f, region,
                                     wall_color, atlas_texture);
            sprite_batch_draw_region(&batch,
                                     vec2_make(PLAYFIELD_LEFT - thickness * 0.5f, 0.0f),
                                     vec2_make(thickness, PLAYFIELD_HEIGHT), 0.0f, region,
                                     wall_color, atlas_texture);
            sprite_batch_draw_region(&batch,
                                     vec2_make(PLAYFIELD_RIGHT + thickness * 0.5f, 0.0f),
                                     vec2_make(thickness, PLAYFIELD_HEIGHT), 0.0f, region,
                                     wall_color, atlas_texture);
            sprite_batch_draw_region(&batch,
                                     vec2_make(0.0f, PLAYFIELD_TOP + thickness * 0.5f),
                                     vec2_make(PLAYFIELD_WIDTH, thickness), 0.0f, region,
                                     wall_color, atlas_texture);
        }

        // 砖块
        {
            const atlas_region_t *region = texture_atlas_region(&atlas, SHAPE_BRICK);
            for (u32 i = 0; i < BRICK_COLUMNS * BRICK_ROWS; ++i) {
                const brick_t *brick = &game.bricks[i];
                if (!brick->alive) {
                    continue;
                }
                sprite_batch_draw_region(&batch, vec2_add(brick->position, shake),
                                         vec2_make(BRICK_WIDTH, BRICK_HEIGHT), 0.0f, region,
                                         brick_color(brick->row), atlas_texture);
            }
        }

        // 挡板
        sprite_batch_draw_region(&batch, vec2_add(game.paddle_position, shake),
                                 vec2_make(game.paddle_width, PADDLE_HEIGHT * 2.2f), 0.0f,
                                 texture_atlas_region(&atlas, SHAPE_PADDLE),
                                 vec4_make(0.85f, 0.90f, 1.0f, 1.0f), atlas_texture);

        // 球
        sprite_batch_draw_region(&batch, vec2_add(game.ball.position, shake),
                                 vec2_splat(game.ball.radius * 2.4f), 0.0f,
                                 texture_atlas_region(&atlas, SHAPE_BALL),
                                 vec4_make(1.0f, 0.95f, 0.75f, 1.0f), atlas_texture);

        // ---- 调试绘制 ----
        if (show_debug) {
            for (u32 i = 0; i < BRICK_COLUMNS * BRICK_ROWS; ++i) {
                const brick_t *brick = &game.bricks[i];
                if (!brick->alive) {
                    continue;
                }
                debug_draw_rect(&debug,
                                vec2_make(brick->position.x - BRICK_WIDTH * 0.5f,
                                          brick->position.y - BRICK_HEIGHT * 0.5f),
                                vec2_make(brick->position.x + BRICK_WIDTH * 0.5f,
                                          brick->position.y + BRICK_HEIGHT * 0.5f),
                                vec4_make(0.0f, 1.0f, 0.4f, 0.8f), false, 1.0f);
            }

            // 球速向量：调"球为什么往这个方向飞"时最有用
            debug_draw_arrow(&debug, game.ball.position,
                             vec2_add(game.ball.position, vec2_scale(game.ball.velocity, 0.15f)),
                             vec4_make(1.0f, 0.3f, 0.3f, 1.0f), 2.0f);

            // 挡板碰撞盒
            debug_draw_rect(&debug,
                            vec2_make(game.paddle_position.x - game.paddle_width * 0.5f,
                                      game.paddle_position.y - PADDLE_HEIGHT * 0.5f),
                            vec2_make(game.paddle_position.x + game.paddle_width * 0.5f,
                                      game.paddle_position.y + PADDLE_HEIGHT * 0.5f),
                            vec4_make(0.3f, 0.6f, 1.0f, 0.9f), false, 1.0f);

            debug_draw_grid(&debug, vec2_make(PLAYFIELD_LEFT, PLAYFIELD_BOTTOM),
                            vec2_make(PLAYFIELD_RIGHT, PLAYFIELD_TOP), 80.0f,
                            vec4_make(0.2f, 0.25f, 0.35f, 0.5f), 1.0f);
        }

        // ---- HUD ----
        {
            const f32 hud_y = PLAYFIELD_TOP + 34.0f;

            text_style_t style = ui_style;
            style.align = TEXT_ALIGN_LEFT;
            style.valign = TEXT_VALIGN_MIDDLE;
            style.color = vec4_make(0.95f, 0.95f, 1.0f, 1.0f);

            text_draw_fmt(&batch, &style, vec2_make(PLAYFIELD_LEFT, hud_y), "分数 %u", game.score);

            style.align = TEXT_ALIGN_CENTER;
            text_draw_fmt(&batch, &style, vec2_make(0.0f, hud_y), "关卡 %u", game.level);

            style.align = TEXT_ALIGN_RIGHT;
            text_draw_fmt(&batch, &style, vec2_make(PLAYFIELD_RIGHT, hud_y), "生命 %u", game.lives);

            // 最高分（左上角小字）
            style.align = TEXT_ALIGN_LEFT;
            style.color = vec4_make(0.6f, 0.65f, 0.8f, 1.0f);
            text_draw_fmt(&batch, &style, vec2_make(PLAYFIELD_LEFT, hud_y + 30.0f), "最高 %u",
                          game.best_score);

            // 中央提示
            center_style.color = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
            switch (game.state) {
            case GAME_STATE_READY:
                text_draw_outlined(&batch, &center_style, vec2_make(0.0f, -60.0f),
                                   vec4_make(0.0f, 0.0f, 0.0f, 0.85f), 2.0f, "按空格发球");
                break;
            case GAME_STATE_PAUSED:
                text_draw_outlined(&batch, &center_style, vec2_make(0.0f, 0.0f),
                                   vec4_make(0.0f, 0.0f, 0.0f, 0.85f), 2.0f, "已暂停\n按 P 继续");
                break;
            case GAME_STATE_GAME_OVER:
                center_style.color = vec4_make(1.0f, 0.45f, 0.45f, 1.0f);
                text_draw_outlined(&batch, &center_style, vec2_make(0.0f, 20.0f),
                                   vec4_make(0.0f, 0.0f, 0.0f, 0.85f), 2.0f, "游戏结束");
                center_style.color = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
                text_draw_outlined_fmt(&batch, &center_style, vec2_make(0.0f, -40.0f),
                                       vec4_make(0.0f, 0.0f, 0.0f, 0.85f), 2.0f,
                                       "最终分数 %u\n按 R 重开", game.score);
                break;
            case GAME_STATE_WIN:
                center_style.color = vec4_make(0.5f, 1.0f, 0.6f, 1.0f);
                text_draw_outlined(&batch, &center_style, vec2_make(0.0f, 20.0f),
                                   vec4_make(0.0f, 0.0f, 0.0f, 0.85f), 2.0f, "关卡通过！");
                center_style.color = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
                text_draw_outlined_fmt(&batch, &center_style, vec2_make(0.0f, -40.0f),
                                       vec4_make(0.0f, 0.0f, 0.0f, 0.85f), 2.0f,
                                       "分数 %u\n按 R 重开", game.score);
                break;
            default:
                break;
            }
        }

        sprite_batch_end(&batch);
        renderer_end_frame(renderer);

        frames_rendered++;

        if (clock.elapsed_seconds - last_title_update > 0.5) {
            last_title_update = clock.elapsed_seconds;
            render_stats_t stats;
            renderer_get_stats(renderer, &stats);
            snprintf(title, sizeof(title),
                     "Polaire Engine M3 打砖块 | 分数 %u | 生命 %u | 绘制 %u | %.0f FPS", game.score,
                     game.lives, stats.draw_calls, clock.fps);
            platform_window_set_title(window, title);
        }

        if (options.frames > 0 && frames_rendered >= options.frames) {
            break;
        }

        frame_limiter_wait(&limiter, frame_start);
    }

    {
        render_stats_t stats;
        renderer_get_stats(renderer, &stats);
        LOG_INFO("共渲染 %u 帧，总时长 %.3f 秒", frames_rendered, clock.elapsed_seconds);
        LOG_INFO("游戏状态: 分数 %u | 生命 %u | 剩余砖块 %u | 状态 %d", game.score, game.lives,
                 game.bricks_alive, (int)game.state);
        LOG_INFO("绘制调用 %u | 三角面 %u | 帧时间 平均 %.3f ms | FPS %.1f", stats.draw_calls,
                 stats.triangles, clock.delta_ms, clock.fps);
    }

    exit_code = 0;

cleanup:
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
    printf("========== M3 打砖块 ==========\n");
    printf("  渲染帧数   : %u\n", frames_rendered);
    printf("  最终分数   : %u\n", game.score);
    printf("  内存泄漏   : %s\n", leaked ? "有" : "无");
    printf("  日志行数   : %llu\n", (unsigned long long)log_lines);
    printf("===============================\n");

    return (exit_code == 0 && !leaked) ? 0 : 1;
}
