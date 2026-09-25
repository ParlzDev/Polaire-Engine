// =============================================================================
//  examples/01_window/main.c —— M1 演示：窗口 + OpenGL 4.5 + 渐变背景
//
//  这个程序验证 M1 的每一条验收标准：
//    * 窗口可创建、缩放、全屏切换（Alt+Enter 或 F11）
//    * OpenGL 4.5 Core Context 创建成功（并把驱动实际给的版本打出来）
//    * 清屏为渐变背景 —— 用 shader 画全屏三角形，不是 glClearColor 纯色，
//      这样 shader / VAO / draw call 整条链路都真的跑通了
//    * 键盘鼠标可读（Tab 切换鼠标锁定，移动鼠标看位移数值）
//    * ESC 退出
//    * 帧率稳定（vsync 或帧率限制器）
//    * 退出时零内存泄漏（沿用 M0 的追踪分配器）
//
//  命令行参数（让它能在 CI 里无人值守跑）：
//      --frames N      跑 N 帧后自动退出（默认 0 = 一直跑直到按 ESC）
//      --hidden        创建窗口但不显示（CI / 远程会话用）
//      --no-vsync      关闭垂直同步，改用帧率限制器
//      --fullscreen    以全屏启动
//      --width N       窗口宽度（默认 1280）
//      --height N      窗口高度（默认 720）
//      --lowres-timer  不提升系统计时器精度（对比帧率稳定性用）
// =============================================================================
#include <stdio.h>
#include <string.h>

#include "engine/engine.h"

// =============================================================================
//  命令行参数
// =============================================================================
typedef struct options_t {
    u32  width;
    u32  height;
    u32  frames;
    bool hidden;
    bool vsync;
    bool fullscreen;
    bool high_resolution_timer;
} options_t;

static void print_usage(void)
{
    printf("用法: 01_window [选项]\n");
    printf("  --frames N       跑 N 帧后退出（0 = 一直跑，默认 0）\n");
    printf("  --hidden         创建窗口但不显示\n");
    printf("  --no-vsync       关闭垂直同步，改用帧率限制器\n");
    printf("  --fullscreen     以全屏启动\n");
    printf("  --width N        窗口宽度（默认 1280）\n");
    printf("  --height N       窗口高度（默认 720）\n");
    printf("  --lowres-timer   不提升系统计时器精度\n");
}

static bool parse_u32(const char *text, u32 *out_value)
{
    if (text == NULL || out_value == NULL) {
        return false;
    }
    u32 value = 0;
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
    options.fullscreen = false;
    options.high_resolution_timer = true;

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
        } else if (strcmp(arg, "--fullscreen") == 0) {
            options.fullscreen = true;
        } else if (strcmp(arg, "--lowres-timer") == 0) {
            options.high_resolution_timer = false;
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
//  最小着色器工具
//
//  这些函数刻意留在示例里，不进引擎 —— M1 的任务边界是"窗口 + 上下文"，
//  shader / buffer 的封装属于 M2 的渲染核心。这里手写一遍是为了让 M1 能被
//  端到端验证，而不是提前把 M2 的抽象做出来再回头改。
// =============================================================================
#define SHADER_LOG_CAPACITY 2048

// glGetShaderInfoLog 返回的 length 不含结尾 NUL，且可能等于缓冲区长度。
// 统一在这里收口，避免每个调用点各写一遍边界判断。
static void terminate_log(char *log, GLsizei length, usize capacity)
{
    usize end = (length > 0 && (usize)length < capacity) ? (usize)length : (capacity - 1u);
    log[end] = '\0';
}

static GLuint compile_shader(const gl_api_t *gl, GLenum type, const char *source,
                             const char *label)
{
    const GLuint shader = gl->glCreateShader(type);
    if (shader == 0) {
        LOG_ERROR("glCreateShader 失败 (%s)", label);
        return 0;
    }

    gl->glShaderSource(shader, 1, &source, NULL);
    gl->glCompileShader(shader);

    GLint status = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        char    log[SHADER_LOG_CAPACITY];
        GLsizei length = 0;
        gl->glGetShaderInfoLog(shader, (GLsizei)sizeof(log), &length, log);
        terminate_log(log, length, sizeof(log));
        LOG_ERROR("%s 着色器编译失败:\n%s", label, log);
        gl->glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint link_program(const gl_api_t *gl, GLuint vertex_shader, GLuint fragment_shader)
{
    const GLuint program = gl->glCreateProgram();
    if (program == 0) {
        LOG_ERROR("glCreateProgram 失败");
        return 0;
    }

    gl->glAttachShader(program, vertex_shader);
    gl->glAttachShader(program, fragment_shader);
    gl->glLinkProgram(program);

    GLint status = 0;
    gl->glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        char    log[SHADER_LOG_CAPACITY];
        GLsizei length = 0;
        gl->glGetProgramInfoLog(program, (GLsizei)sizeof(log), &length, log);
        terminate_log(log, length, sizeof(log));
        LOG_ERROR("着色器程序链接失败:\n%s", log);
        gl->glDeleteProgram(program);
        return 0;
    }

    return program;
}

// 全屏三角形：不用顶点缓冲，靠 gl_VertexID 算出三个顶点。
// 顶点 (0,0) (2,0) (0,2) 覆盖整个 [-1,1] 的裁剪空间，且比"两个三角形拼的
// 全屏 quad"少一次对角线接缝（那条接缝在某些驱动上会有可见的插值瑕疵）。
static const char *const k_vertex_shader_body =
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const k_fragment_shader_body =
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform float u_time;\n"
    "void main() {\n"
    "    vec3 bottom = vec3(0.05, 0.08, 0.20);\n"
    "    vec3 top    = vec3(0.62, 0.24, 0.48);\n"
    "    vec3 color  = mix(bottom, top, v_uv.y);\n"
    // 随时间横向流动的暗纹：肉眼能立刻确认"画面确实在动"，
    // 而不是"程序卡死了但恰好画面是静态的"。
    "    color += 0.035 * sin((v_uv.x * 3.0 + u_time * 0.35) * 6.2831853);\n"
    "    frag_color = vec4(color, 1.0);\n"
    "}\n";

// =============================================================================
//  main
// =============================================================================
int main(int argc, char **argv)
{
    const options_t options = parse_options(argc, argv);

    // 用追踪分配器包住堆，结束时证明零泄漏。
    // 窗口和 GL 上下文都是通过它分配的，所以这条断言能覆盖到 M1 新增的
    // 全部动态分配路径。
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    if (!log_init("01_window.log", LOG_LEVEL_DEBUG)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }

    LOG_INFO("MyEngine %s —— M1 窗口 + OpenGL", MYENGINE_VERSION_STRING);
    LOG_INFO("参数: %ux%u, frames=%u, vsync=%s, hidden=%s, fullscreen=%s",
             options.width, options.height, options.frames, options.vsync ? "on" : "off",
             options.hidden ? "yes" : "no", options.fullscreen ? "yes" : "no");

    if (!platform_time_init(options.high_resolution_timer)) {
        LOG_ERROR("计时系统初始化失败");
        log_shutdown();
        return 1;
    }
    LOG_DEBUG("计时器: %llu Hz，高精度模式 %s",
              (unsigned long long)platform_time_frequency(),
              platform_time_is_high_resolution() ? "已启用" : "未启用");

    int exit_code = 1;
    platform_window_t *window = NULL;
    gl_context_t      *context = NULL;
    const gl_api_t    *gl = NULL; // 声明在最前面：cleanup 标签会用到它
    GLuint             vao = 0;
    GLuint             program = 0;
    u32                frames_rendered = 0; // 同样要在 goto 之前声明并初始化

    // ---- 窗口 ----
    platform_window_desc_t window_desc = platform_window_desc_default();
    window_desc.title = "MyEngine M1 —— 窗口 + OpenGL";
    window_desc.width = options.width;
    window_desc.height = options.height;
    window_desc.visible = !options.hidden;
    window_desc.fullscreen = options.fullscreen;
    window_desc.resizable = true;
    window_desc.enable_high_dpi = true;

    if (!platform_window_create(&tracker.base, &window_desc, &window)) {
        LOG_ERROR("窗口创建失败");
        goto cleanup;
    }

    {
        u32 width = 0;
        u32 height = 0;
        platform_window_get_size(window, &width, &height);
        LOG_INFO("窗口就绪: 客户区 %ux%u，DPI 缩放 %.2f", width, height,
                 platform_window_dpi_scale(window));
    }

    // ---- OpenGL 上下文 ----
    gl_context_desc_t context_desc = gl_context_desc_default();
    context_desc.vsync = options.vsync;

    if (!gl_context_create(&tracker.base, window, &context_desc, &context)) {
        LOG_ERROR("OpenGL 上下文创建失败。");
        LOG_ERROR("如果是远程桌面或虚拟机，请确认已启用硬件 3D 加速。");
        goto cleanup;
    }

    LOG_INFO("OpenGL %u.%u  |  %s", gl_context_major_version(context),
             gl_context_minor_version(context), gl_context_version_string(context));
    LOG_INFO("厂商      : %s", gl_context_vendor_string(context));
    LOG_INFO("渲染器    : %s", gl_context_renderer_string(context));
    LOG_INFO("GLSL      : %s", gl_context_glsl_version_string(context));
    LOG_INFO("垂直同步  : %s", gl_context_get_vsync(context) ? "开" : "关");
    LOG_INFO("调试输出  : %s", gl_context_has_debug_output(context) ? "可用" : "不可用");

    if (gl_context_major_version(context) < 4 || gl_context_minor_version(context) < 5) {
        LOG_WARN("驱动给出的版本低于请求的 4.5。M1 仍可运行，"
                 "但 M2 之后的渲染核心需要 4.5 特性，请更新显卡驱动。");
    }

    gl = gl_api_load();
    if (gl == NULL) {
        LOG_ERROR("OpenGL 函数表加载失败");
        goto cleanup;
    }

    // ---- 着色器 ----
    // 版本行按上下文实际版本生成，这样即使降级到 4.3 也能编译。
    //
    // 缓冲区必须装得下"版本行 + 整个着色器正文"。这里踩过一次坑：
    // 缓冲区给 128 字节，而正文有 180 多字节，snprintf 静默截断，
    // 驱动报的是 "syntax error, unexpected $end" —— 看起来像着色器语法错，
    // 实际上是源码根本没传完整。截断错误永远不会指向真正的原因。
    char vertex_source[1024];
    char fragment_source[1024];
    snprintf(vertex_source, sizeof(vertex_source), "#version %u%u0 core\n%s",
             gl_context_major_version(context), gl_context_minor_version(context),
             k_vertex_shader_body);
    snprintf(fragment_source, sizeof(fragment_source), "#version %u%u0 core\n%s",
             gl_context_major_version(context), gl_context_minor_version(context),
             k_fragment_shader_body);

    // 显式验证没有发生截断。snprintf 的返回值是"本该写入的长度"，
    // 大于等于缓冲区大小就说明被截断了。
    {
        const int needed_vertex = snprintf(NULL, 0, "#version %u%u0 core\n%s",
                                           gl_context_major_version(context),
                                           gl_context_minor_version(context),
                                           k_vertex_shader_body);
        if (needed_vertex < 0 || (usize)needed_vertex >= sizeof(vertex_source)) {
            LOG_ERROR("顶点着色器源码缓冲区不足（需要 %d 字节，只有 %llu）", needed_vertex,
                      (unsigned long long)sizeof(vertex_source));
            goto cleanup;
        }
    }

    const GLuint vertex_shader = compile_shader(gl, GL_VERTEX_SHADER, vertex_source, "顶点");
    if (vertex_shader == 0) {
        goto cleanup;
    }
    const GLuint fragment_shader =
        compile_shader(gl, GL_FRAGMENT_SHADER, fragment_source, "片元");
    if (fragment_shader == 0) {
        gl->glDeleteShader(vertex_shader);
        goto cleanup;
    }

    program = link_program(gl, vertex_shader, fragment_shader);
    gl->glDeleteShader(vertex_shader);
    gl->glDeleteShader(fragment_shader);
    if (program == 0) {
        goto cleanup;
    }

    // Core Profile 要求至少绑定一个 VAO 才能发起绘制，
    // 即使这个绘制不用任何顶点属性（我们靠 gl_VertexID 取顶点）。
    gl->glGenVertexArrays(1, &vao);
    if (vao == 0) {
        LOG_ERROR("glGenVertexArrays 失败");
        goto cleanup;
    }

    const GLint uniform_time = gl->glGetUniformLocation(program, "u_time");
    if (uniform_time < 0) {
        // 不是致命错误，但说明 uniform 名写错了或者被优化掉了。
        LOG_WARN("找不到 uniform \"u_time\"，画面将不会随时间变化");
    }

    LOG_INFO("渲染资源就绪: program=%u, vao=%u", (unsigned)program, (unsigned)vao);

    // ---- 帧循环 ----
    frame_clock_t   clock;
    frame_limiter_t limiter;
    frame_clock_init(&clock);
    frame_limiter_init(&limiter, 60.0);

    // vsync 打开时帧率已经被 SwapBuffers 锁住，再加一层限制器只会
    // 让帧时间变成"两个限制的叠加"，反而更不稳。
    if (options.vsync) {
        limiter.enabled = false;
    }

    if (limiter.enabled) {
        LOG_INFO("帧率限制器: 目标 %.1f FPS，预算 %.2f ms，忙等窗口 %.2f ms",
                 limiter.target_fps, limiter.budget_ms, limiter.spin_margin_ms);
    }

    // 在进入循环前重置，让第一帧的 delta 就是真实帧耗时，
    // 而不是"初始化花掉的时间"。
    frame_clock_reset(&clock);

    char title[256];
    f64  last_title_update = 0.0;
    bool running = true;

    while (running) {
        const platform_time_point_t frame_start = platform_time_now();

        // 消息泵 + 输入状态更新
        if (!platform_window_poll_events(window)) {
            LOG_INFO("收到退出请求");
            break;
        }

        const input_state_t *input = platform_window_input(window);

        if (input_key_pressed(input, KEY_ESCAPE)) {
            LOG_INFO("按下 ESC，退出");
            break;
        }
        if (input_key_pressed(input, KEY_F11)) {
            const bool fullscreen = !platform_window_is_fullscreen(window);
            platform_window_set_fullscreen(window, fullscreen);
            LOG_INFO("全屏: %s", fullscreen ? "开" : "关");
        }
        if (input_key_pressed(input, KEY_TAB)) {
            const bool locked = !platform_window_is_mouse_locked(window);
            platform_window_set_mouse_locked(window, locked);
            LOG_INFO("鼠标锁定: %s", locked ? "开" : "关");
        }

        const f64 delta = frame_clock_tick(&clock);

        // ---- 绘制 ----
        u32 width = 0;
        u32 height = 0;
        platform_window_get_size(window, &width, &height);
        if (width == 0 || height == 0) {
            // 最小化时客户区为 0，跳过绘制（glViewport 宽度为 0 是合法但无意义的）
            frame_limiter_wait(&limiter, frame_start);
            continue;
        }

        gl->glViewport(0, 0, (GLsizei)width, (GLsizei)height);
        gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);

        gl->glUseProgram(program);
        if (uniform_time >= 0) {
            gl->glUniform1f(uniform_time, (GLfloat)clock.elapsed_seconds);
        }
        gl->glBindVertexArray(vao);
        gl->glDrawArrays(GL_TRIANGLES, 0, 3);

        gl_context_swap_buffers(context);

        frames_rendered++;

        // 窗口标题每 0.25 秒更新一次。每帧都更新会让 SetWindowTextW
        // 成为帧时间的一部分（它要过一遍消息队列），实测能吃掉几百微秒。
        if (clock.elapsed_seconds - last_title_update > 0.25) {
            last_title_update = clock.elapsed_seconds;
            snprintf(title, sizeof(title), "MyEngine M1 | %.1f FPS | %.2f ms | GL %u.%u | 帧 %u",
                     clock.fps, clock.delta_ms, gl_context_major_version(context),
                     gl_context_minor_version(context), frames_rendered);
            platform_window_set_title(window, title);
        }

        if (options.frames > 0 && frames_rendered >= options.frames) {
            LOG_INFO("已达到指定帧数 %u，退出", options.frames);
            break;
        }

        // 帧率限制放在最后：此时本帧的"渲染工作"已完成，
        // 等待补齐剩下的预算即可让帧周期稳定在目标值。
        frame_limiter_wait(&limiter, frame_start);

        (void)delta;
        if (input->focus_lost) {
            LOG_DEBUG("窗口失去焦点");
        }
    }

    // ---- 总结 ----
    LOG_INFO("共渲染 %u 帧，总时长 %.3f 秒", frames_rendered, clock.elapsed_seconds);
    if (frames_rendered > 1) {
        LOG_INFO("帧时间: 平均 %.3f ms | 最小 %.3f ms | 最大 %.3f ms | 平滑 FPS %.1f",
                 clock.delta_ms, clock.min_delta_ms, clock.max_delta_ms, clock.fps);
    }

    exit_code = 0;

cleanup:
    // 释放顺序：GL 资源 -> GL 上下文 -> 窗口 -> 计时系统。
    // GL 资源必须在上下文还活着的时候删（glDeleteProgram 需要 current 上下文）；
    // 上下文又必须在窗口之前销毁，因为它借用窗口的 DC。
    if (context != NULL && gl != NULL) {
        gl_context_make_current(context);
        if (vao != 0) {
            gl->glDeleteVertexArrays(1, &vao);
        }
        if (program != 0) {
            gl->glDeleteProgram(program);
        }
    }

    gl_context_destroy(context);
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
    printf("========== M1 验收 ==========\n");
    printf("  窗口 / 上下文    : %s\n", exit_code == 0 ? "创建成功" : "失败");
    printf("  渲染帧数         : %u\n", frames_rendered);
    printf("  内存泄漏         : %s\n", leaked ? "有" : "无");
    printf("  日志行数         : %llu\n", (unsigned long long)log_lines);
    printf("=============================\n");

    return (exit_code == 0 && !leaked) ? 0 : 1;
}
