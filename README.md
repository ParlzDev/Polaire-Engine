# MyEngine（北极星）

Windows 优先、纯 C11、2D/3D/2.5D 三模式统一的游戏引擎。

**当前进度：M3 已完成**（M0 地基 → M1 窗口/GL → M2 渲染核心 → M3 2D 管线）

---

## 一分钟上手

```bash
source tools/env.sh          # 把本机 clang / cmake / ninja 加进 PATH

cmake --preset debug
cmake --build --preset debug
ctest --preset debug          # 或直接跑 build/debug/tests/engine_tests.exe
```

跑示例：

```bash
./build/debug/examples/00_smoke/00_smoke.exe                  # M0：内存/容器/数学
./build/debug/examples/01_window/01_window.exe                # M1：开窗 + 渐变背景
./build/debug/examples/02_render/02_render.exe                # M2：3D 渲染核心
./build/debug/examples/03_sprite/03_sprite.exe                # M3：1 万精灵压测
./build/debug/examples/04_breakout/04_breakout.exe            # M3：打砖块（可玩）
./build/debug/examples/02_render/02_render.exe --help         # 查看命令行参数
```

`03_sprite` 的按键：`1`-`4` 切换精灵数量（1k/10k/50k/上限），`F1` 调试绘制，
空格暂停，左键拖动平移，滚轮缩放。

`04_breakout` 的按键：鼠标或 `A`/`D` 移动挡板，空格发球，`P` 暂停，`R` 重开，
`F1` 调试绘制（碰撞盒 + 球速向量）。

**性能测试**（关闭 vsync 与帧率限制器）：

```bash
./build/release/examples/03_sprite/03_sprite.exe --frames 300 --hidden --no-vsync --unlimited --count 10000
```

`02_render` 的操作：`ESC` 退出，`1`/`2` 切换透视/正交，`3` 线框，`4` 参考网格，
左键拖动环绕相机，滚轮缩放。加 `--obj <路径>` 可以加载自己的 OBJ 模型。

所有示例都支持无人值守运行（CI 用）：

```bash
# 隐藏窗口跑 300 帧后自动退出，并打印帧时间与统计
./build/debug/examples/02_render/02_render.exe --frames 300 --hidden
```

提交前自检（警告视为错误）：

```bash
cmake --build --preset werror
```

---

## 目录结构

```
include/engine/
  engine.h                 总入口
  core/                    基础层（不依赖平台）
    types.h                固定宽度类型、通用宏、handle_t 打包句柄
    assert.h               ASSERT / ASSERT_MSG / VERIFY / ENGINE_UNREACHABLE
    console.h              控制台 ANSI 能力探测     ← 端口，实现在平台层
    thread.h               互斥量                   ← 端口，实现在平台层
    log.h                  分级日志（控制台 + 文件 + sink 回调）
    memory.h               allocator_t / arena_t / pool_t / arena 适配器
    array.h / hash.h       动态数组 / 开放寻址哈希表
    math.h / vec.h / mat.h / quat.h / transform.h
  platform/                平台层
    window.h               窗口（上层永远不接触 HWND）
    input.h                输入状态（双缓冲 + 事件注入）
    time.h                 计时、帧时钟、帧率限制器
  render/                  渲染层
    render_types.h         后端无关的类型与预设（句柄、格式、管线状态）
    render_backend.h       后端抽象接口（按 Vulkan 语义）
    uniforms.h             uniform 块（std140）+ GLSL 声明（单一来源）
    gl_types.h / gl_api.h  GL 类型与函数表（自写 loader）
    gl_context.h           GL 上下文（两阶段 WGL 引导）
    camera.h               相机（正交 / 透视）
    render_queue.h         渲染队列与排序
    shader.h / texture.h / mesh.h / material.h
    obj_loader.h           Wavefront OBJ 解析
    sprite.h / sprite_batch.h    精灵与批渲染器
    texture_atlas.h        纹理图集（货架打包）
    camera_2d.h            2D 相机（正交 + 像素对齐）
    debug_draw.h           2D 调试绘制
    font.h / text.h        字体与文本渲染
    tilemap.h              图块地图
    renderer.h             引擎级外观（业务代码只跟它打交道）
    backend/opengl/gl_backend.h   GL 后端工厂

src/core/                  基础层实现
src/platform/win32/        平台层实现（换平台只改这里）
src/render/                渲染层实现
src/render/backend/opengl/ GL 后端实现
src/render/third_party/    stb_image 实现单元（单独关告警）

third_party/stb_image.h    单头库（图片解码）
third_party/stb_truetype.h 单头库（字体光栅化）
third_party/font8x8_basic.h 公有领域 8x8 位图字体
assets/models/             测试用 OBJ 模型

tests/                     零依赖单元测试框架 + 199 个用例
examples/00_smoke/         M0 冒烟示例（含零泄漏验证）
examples/01_window/        M1 演示
examples/02_render/        M2 演示（3D 渲染核心）
examples/03_sprite/        M3 演示（1 万精灵压测）
examples/04_breakout/      M3 验证游戏（打砖块）

docs/M0-设计决策.md         M0 全部设计决策与踩过的坑
docs/M1-设计决策.md         M1 全部设计决策与踩过的坑
docs/M2-设计决策.md         M2 全部设计决策与踩过的坑
docs/M3-设计决策.md         M3 全部设计决策与踩过的坑
AI_PROMPT.md               与 AI 协作时的常驻总纲
prompts/                   各阶段提示词
tools/env.sh, env.cmd      工具链 PATH 注入脚本
```

---

## 验收结果

### M0 地基

| 验收项 | 要求 | 实际 |
|---|---|---|
| 构建 | CMake + Ninja + clang 通过 | ✅ Debug / Release / `-Werror` 三档零告警 |
| 数学库测试 | 矩阵乘、逆、四元数旋转、投影全过 | ✅ |
| arena / pool 无泄漏 | 可验证 | ✅ 追踪分配器断言 `live_blocks == 0` |
| 日志 | 分级输出到控制台 + 文件 | ✅ 控制台带色、文件 64KB 缓冲、WARN 以上带 `file:line` |
| ASSERT | Debug 生效、Release 编译掉 | ✅ 由 CMake 按配置注入 `ENGINE_DEBUG` |
| 语言 | 纯 C11 | ✅ `-std=c11`（关闭 GNU 扩展） |

### M1 窗口 + OpenGL

| 验收项 | 要求 | 实际 |
|---|---|---|
| OpenGL 版本 | 4.5 Core Context | ✅ 实测 4.5.0 NVIDIA 616.56（RTX 5060） |
| 渐变背景 | 用 shader 画，不是 `glClearColor` | ✅ 全屏三角形 + GLSL 450 |
| 窗口 | 可缩放、可全屏切换 | ✅ 全屏后客户区 1280x720 → 1920x1080 |
| 输入 | 键鼠可读、鼠标可锁定 | ✅ `Tab` 切换锁定，位移不受窗口边界限制 |
| 退出 | ESC | ✅ |
| 帧率 | 稳定 60 | ✅ vsync：平均 16.665ms；限制器：平均 16.670ms / 最大 16.723ms |
| 高 DPI | 支持 | ✅ `PER_MONITOR_AWARE_V2` + `WM_DPICHANGED` |
| Unicode | 用 W API | ✅ `CreateWindowExW` / `SetWindowTextW` |

### M2 渲染核心

| 验收项 | 要求 | 实际 |
|---|---|---|
| 画三角形 | ✅ | 网格地面 + 参考网格线（LINE_LIST） |
| 贴图 quad | ✅ | 程序化渐变纹理 + 无光照管线 |
| 加载 OBJ | ✅ | 内置八面体 + 文件加载（`assets/models/cube.obj`） |
| 相机切换 | 正交 / 透视 | ✅ 运行中按 `1`/`2` 切换 |
| 后端抽象 | 按 Vulkan 语义、无 GL 类型 | ✅ `pipeline_desc_t` / 显式 render pass / UBO |
| 上层不调 GL | 示例里搜不到 `gl->` | ✅ |
| 帧内零 malloc | 可验证 | ✅ 追踪分配器断言队列复用内存 |
| 单元测试 | — | ✅ 161 用例 / 17626 断言 / 0 失败 |
| 运行时 | 无 GL 错误 | ✅ 开着 `GL_KHR_debug` 跑 300 帧，零错误 |

### M3 2D 渲染管线

| 验收项 | 要求 | 实际 |
|---|---|---|
| 1 万精灵 | < 16ms | ✅ Release **0.925 ms**（17 倍余量）；Debug 2.282 ms |
| 5 万精灵 | — | ✅ Release 3.284 ms |
| draw call | 个位数 | ✅ 3 次（图集 1 + 字体 1 + 调试 1）；1 万精灵本身只占 1 次 |
| 打砖块 | 可玩 | ✅ 挡板跟手、球不穿墙（子步进）、分数/生命/暂停/重开/关卡 |
| 文本 | 中英文 | ✅ 微软雅黑 TTF（119 字形预光栅化）；无字体时退回内置 8x8 |
| 2D 调试绘制 | — | ✅ 线 / 矩形 / 旋转矩形 / 圆 / 十字 / 箭头 / 网格 |
| 单元测试 | — | ✅ 199 用例 / 18672 断言 / 0 失败 |
| 运行时 | 无 GL 错误 | ✅ 零错误、零内存泄漏 |

---

## 关键约定（改代码前先看）

### 坐标系

| 项 | 约定 |
|---|---|
| 世界空间 | 右手系，Y 轴向上 |
| 相机 | 默认看向 **-Z** |
| 裁剪空间 | X/Y ∈ [-1, 1] |
| **深度范围** | **[0, 1]**（不是 OpenGL 默认的 [-1, 1]） |

深度取 [0,1] 是为了对齐 Vulkan / D3D 的原生约定。OpenGL 4.5 用
`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` 一次性对齐，不需要修正矩阵。
**"按 Vulkan 语义设计，让 GL 去适配"** —— 这是 M13 多后端能否低成本落地的关键。

### 分层

```
core  →  platform  →  render  →  （业务代码）
```

上层可以包含下层，反过来不行。

**唯一的例外是 core 自己声明的"端口"**：`core/console.h` 与 `core/thread.h`
声明接口，实现在 `src/platform/win32/`。

渲染层内部还有一层：`renderer` → `render_backend`，
**后端不允许依赖 renderer**（资源上限宏因此放在 `render_types.h`）。

### 渲染后端的三条铁律

1. **接口里不出现任何图形 API 类型**（`GLuint` / `VkImage` / `ID3D11*` 都不行）。
2. **状态是打包成 desc 一次性提交的**，不是逐项设置 —— Vulkan 的管线状态对象
   是不可变的，逐项设置在 Vulkan 上无法实现。
3. **uniform 是"块 + 绑定点"**，不是"按名字" —— Vulkan 没有按名字设 uniform。

### 内存

* 帧内零 `malloc`：临时数据走 `arena_t`，`arena_reset()` 后复用块。
* 永久对象走 `pool_t`，句柄带代际号，悬空句柄返回 `NULL` 而不是野指针。
* 所有"会分配内存"的模块接收 `allocator_t*` 而不是直接调 `malloc`，
  这样测试里能换成追踪分配器，把"无泄漏"变成可执行断言。

### 命名

* `snake_case`，类型加 `_t` 后缀，宏全大写。
* 公开 API 加模块前缀：`arena_`、`vec3_`、`log_`、`platform_`、`gl_`、`render_`。
* 内部函数加 `static`。
* 原始需求里的容器 `vec` 已改名为 `array`，避免与数学库的 `vec2/vec3/vec4` 撞名。

### 数学库为什么是 header-only

向量运算每条只有几条指令，函数调用开销占比极高，只有放在头文件里让编译器
看到调用点才能真正内联。这是有意偏离"接口与实现分离"的约定，理由与代价
记在 `docs/M0-设计决策.md`。

---

## 下一步（M4）

3D 渲染管线：多光源、PBR 光照、阴影贴图、天空盒与雾、视锥剔除、
渲染目标抽象（离屏 + MSAA 解析），并**还掉 M3 欠下的 sRGB 账**
（M2 的 3D 用 sRGB 纹理但帧缓冲不编码，M3 的 2D 干脆用 UNORM 纹理）。

验收：模型查看器（多光源 + 阴影 + 天空盒）、sRGB 自检模式。

交付物与验收标准见 `prompts/M4_prompt.md`。


