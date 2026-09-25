# 当前阶段：M1 —— Win32 窗口 + OpenGL 上下文

**状态：已完成（2026-09-24）**

> 用法：粘贴 `AI_PROMPT.md` 全文 + 本文件全文给 AI。

---

## 完成情况

| 验收项 | 实际 |
|---|---|
| OpenGL 4.5 Core Context | ✅ 4.5.0 NVIDIA 616.56（RTX 5060），GLSL 4.50，调试输出可用 |
| 渐变背景（shader 画全屏三角形） | ✅ |
| 窗口可缩放 / 全屏切换 | ✅ Alt+Enter / F11，全屏后客户区 1280x720 → 1920x1080 |
| 键鼠输入 + 鼠标锁定 | ✅ Tab 切换，位移不受窗口边界限制 |
| ESC 退出 | ✅ |
| 帧率稳定 60 | ✅ vsync：平均 16.665ms / 最大 19.210ms；限制器：平均 16.670ms / 最大 16.723ms |
| 高 DPI | ✅ `PER_MONITOR_AWARE_V2` + `WM_DPICHANGED` |
| Unicode API | ✅ `CreateWindowExW` / `SetWindowTextW` |
| 单元测试 | ✅ 129 用例 / 17256 断言 / 0 失败 |
| M0 遗留项清理 | ✅ `console.c` 迁到 platform 层；日志自旋锁换成 Win32 互斥量 |

## 实测踩到的坑（预判之外的）

1. **着色器源码缓冲区截断，报的却是"语法错误"**
   `snprintf` 把 180 多字节的着色器正文截断到 128 字节，驱动报
   `syntax error, unexpected $end, expecting "::" at token "<EOF>"`。
   看起来像着色器写错了，实际上是源码没传完整。缓冲区开到 1024，
   并用 `snprintf(NULL, 0, ...)` 显式验证没有截断。

2. **`input_clear_all` 只清 current 会产生不了 released 边沿**
   当"按下"和"失焦"发生在同一帧时，previous 里还是更早的状态（0），
   于是 released 检测不到。表现是"按住鼠标拖拽时 alt-tab 出去，物品粘在光标上"。
   修法是先把 current 快照进 previous 再清空。

3. **`goto cleanup` 跳过的变量是未初始化的**
   clang 报 `-Wsometimes-uninitialized`。所有会被 `cleanup` 用到的变量
   必须声明在第一个 `goto` 之前。

4. **`ARENA_NEW` 只能用在 arena 上**
   写成 `ARENA_NEW(allocator_heap(), ...)` 是类型错误 ——
   `allocator_heap()` 返回 `allocator_t*`，而宏需要 `arena_t*`。

5. **`wglGetProcAddress` 会返回假指针**
   某些驱动在函数不存在时返回 1 / 2 / 3 / -1 而不是 NULL。
   不判这几个值会在调用时跳到非法地址直接崩。

## 预判坑的命中情况

| # | 预判 | 结果 |
|---|---|---|
| 1 | 像素格式与 context 版本 | ✅ 确实需要两阶段引导 |
| 2 | `CS_OWNDC` 与双缓冲 | ✅ 用了 `CS_OWNDC`，与 GL 要求一致 |
| 3 | `WM_SIZE` 期间调 GL | ✅ 已在消息里只更新尺寸，GL 调用放在帧循环 |
| 4 | 全屏用无边框窗口 | ✅ 采用该方案，alt-tab 友好 |
| 5 | 高 DPI 必须在建窗口前设置 | ✅ |
| 6 | Alt+Enter 触发 `SC_KEYMENU` | ✅ 在 `WM_SYSCOMMAND` 里吞掉 |
| 7 | `WM_CLOSE`/`WM_DESTROY` 顺序 | ✅ `WM_CLOSE` 只标记，销毁由上层控制 |
| 8 | `PeekMessage` 返回值语义 | ✅ 显式判断 `WM_QUIT` |
| 9 | 输入双缓冲 | ✅ |
| 10 | GL 函数调用约定 `__stdcall` | ✅ 每个函数指针都带 `GLAPI` |
| 11 | 垂直同步 | ✅ `wglSwapIntervalEXT(1)`，实测帧时间 16.665ms |
| 12 | `QueryPerformanceFrequency` 只调一次 | ✅ 缓存 |

---

## 本阶段目标

创建 Win32 窗口，初始化 OpenGL 4.5 Core Context，实现消息循环、输入采集、
计时、帧率控制，能开窗画渐变背景并按 ESC 退出。

## 本阶段验收标准

* 窗口可创建、缩放、全屏切换。
* OpenGL 4.5 Core Context 创建成功（运行期查询版本并校验）。
* 清屏为渐变背景（用 shader 画全屏 quad，不是 `glClearColor` 纯色）。
* 键盘/鼠标输入可读取，且支持"上一帧/当前帧"双状态（用于边沿检测）。
* ESC 退出。
* 帧率稳定 60，用 `QueryPerformanceCounter` 计时。
* 支持高 DPI（`SetProcessDpiAwareness`）。
* 用 Unicode API（`CreateWindowExW`）。

## 本阶段技术约束

* 纯 C，Win32 API。
* OpenGL 函数用自写 loader（可以用 glad，但要说明理由）。
* 窗口抽象为 `platform_window_t`，上层不直接碰 `HWND`。
* 输入抽象为 `input_state_t`。
* 平台层只封装系统调用，不含渲染逻辑。
* **把 M0 遗留的 `console.c` 迁到 `src/platform/win32/`。**
* **日志的自旋锁换成 Win32 互斥量**（M0 遗留项，见 docs/M0-设计决策.md 第八节）。

## 本阶段交付物

```
include/engine/platform/platform.h
include/engine/platform/window.h
include/engine/platform/input.h
include/engine/platform/time.h
src/platform/win32/win32_window.c
src/platform/win32/win32_input.c
src/platform/win32/win32_time.c
src/platform/win32/win32_console.c      ← 从 src/core/console.c 迁过来
src/render/backend/opengl/gl_loader.c
examples/01_window/main.c
tests/test_platform.c                   ← 至少覆盖输入状态机与时间换算
```

## 请帮我

1. 先给出 `platform.h` 的完整接口设计（窗口 / 输入 / 计时三块）。
2. 再给出 `win32_window.c` 的实现。
3. 给出 OpenGL loader 的实现方案（含函数指针加载失败的降级策略）。
4. 给出示例 `main.c`（开窗 + 渐变 quad + ESC 退出 + 帧率显示）。
5. 指出 Win32 + OpenGL 的常见坑。

## 不要

* 不要用 SDL / GLFW。
* 不要用 C++。
* 不要写伪代码。
* 不要提前实现 M2 的渲染核心（本阶段只需要画一个全屏 quad）。

---

## 我预判的坑（请逐条确认或纠正）

1. **像素格式与 context 版本**：`wglCreateContextAttribsARB` 必须在
   "临时 context" 上取得函数指针后才能调用。临时 context 的创建/销毁顺序
   写错会得到一个"能创建但版本不对"的 context。
2. **`CS_OWNDC` 与双缓冲**：OpenGL 窗口通常需要 `CS_OWNDC`，
   但 `CS_OWNDC` 和 `CS_CLASSDC` 不能混用。
3. **`WM_SIZE` 期间的 `glViewport`**：在 `WM_SIZE` 里直接调用 GL 函数，
   如果此时 context 未被 make current，会静默失败。
4. **全屏切换**：用 `ChangeDisplaySettingsEx` 还是无边框全屏窗口？
   后者更现代、alt-tab 更友好，但需要处理 `WM_NCCALCSIZE`。
5. **高 DPI**：`SetProcessDpiAwarenessContext` 必须在**创建任何窗口之前**调用，
   否则 DPI 变化会导致窗口尺寸与客户区尺寸对不上（表现为画面被拉伸或裁剪）。
6. **Alt+Enter**：默认会触发 `WM_SYSCOMMAND` 的 `SC_KEYMENU`，
   需要显式处理，否则按 Alt 会卡住消息循环。
7. **`WM_CLOSE` / `WM_DESTROY` / `PostQuitMessage` 的顺序**：
   `WM_CLOSE` 默认调用 `DestroyWindow`，进而产生 `WM_DESTROY`，
   在 `WM_DESTROY` 里 `PostQuitMessage`。在 `WM_CLOSE` 里就
   `PostQuitMessage` 会导致资源释放顺序错乱。
8. **消息循环的 `GetMessage` vs `PeekMessage`**：游戏需要 `PeekMessage`
   非阻塞轮询，但要注意 `WM_QUIT` 的返回值为 0 与 `PeekMessage` 返回 0
   （队列为空）语义不同，必须区分，否则会变成"忙等"或"提前退出"。
9. **输入双缓冲**：直接在 `WndProc` 里写 `current` 状态会导致
   "一帧内多次按键只记录最后一次"，且无法做边沿检测。
   需要 `previous` / `current` 两份状态，每帧开头交换。
10. **GL 函数指针的调用约定**：Win32 下 GL 函数是 `__stdcall`（`APIENTRY`），
    用普通函数指针声明会在 x86 上栈失衡（x64 上恰好一致，所以本地测不出来）。
11. **垂直同步**：`wglSwapIntervalEXT(1)` 才能锁 60FPS；不设置的话
    帧率会跑满，测出来的"稳定 60"是假的。
12. **`QueryPerformanceFrequency` 只调用一次**并缓存，
    每次帧都调用它会带来不必要的开销（虽然很小，但没意义）。

## 本阶段的"完成"定义

示例程序 `01_window` 能：
* 开出一个可缩放、可全屏（Alt+Enter）的窗口；
* 画面是全屏 quad 渲染的渐变背景（说明 shader / VAO / VBO 链路通了）；
* 控制台/日志显示实时帧率与帧时间；
* 按 ESC 正常退出，且退出时无资源泄漏（沿用 M0 的追踪分配器验证）；
* `cmake --build --preset werror` 零告警。
