# 当前阶段：M2 —— 渲染核心

**状态：已完成（2026-09-24）**

> 用法：粘贴 `AI_PROMPT.md` 全文 + 本文件全文给 AI。

---

## 完成情况

| 验收项 | 实际 |
|---|---|
| 画三角形 | ✅ 网格地面 + 参考网格线（LINE_LIST） |
| 贴图 quad | ✅ 程序化渐变纹理 + 无光照管线 |
| 加载 OBJ | ✅ 内置八面体 + 文件加载（`assets/models/cube.obj`，去重率 33%） |
| 相机正交 / 透视 | ✅ 运行中按 `1`/`2` 切换 |
| 后端按 Vulkan 语义 | ✅ `pipeline_desc_t` 不可变状态对象 + 显式 render pass + UBO |
| 接口里无 GL 类型 | ✅ 示例里搜不到 `gl->` |
| 资源用句柄 | ✅ 两层句柄（renderer 池 → 后端句柄） |
| 帧内零 malloc | ✅ 追踪分配器断言队列复用内存 |
| 单元测试 | ✅ 161 用例 / 17626 断言 / 0 失败 |
| 运行时 | ✅ 开着 `GL_KHR_debug` 跑 300 帧，零 GL 错误、零内存泄漏 |

## 实测踩到的坑（预判之外的）

1. **`GL_TEXTURE_WRAP_R` 手抄成 `0x8064`**（正确是 `0x8072`）。
   手写 GL 常量表的典型风险：值抄错不会编译失败，只在运行期由驱动的调试回调
   报 `GL_INVALID_ENUM`。**M1 引入的 `glDebugMessageCallback` 在 M2 立刻赚回了成本**。

2. **`gl_api_load` 把"失败"也缓存了**。
   `gl_backend_init` 在创建上下文**之前**调了一次，此时没有 current 上下文，
   `wglGetProcAddress` 全返回 NULL，整张残缺的表被永久缓存 ——
   报"49 个必需函数加载失败"，看起来像驱动问题。
   修法：失败不缓存 + 把 `gl_api_load()` 移到 `gl_context_create` 之后。

3. **`vertex_format_byte_size` 对紧凑格式算错**。
   写成"分量数 × 4"，于是 `UNORM8X4` 和 `UNORM16X4` 都算成 16（应为 4 和 8）。
   顶点布局校验用的是同一个错值所以照样通过，但实际数据错位、画面一片混乱。
   测试只覆盖 float 格式的话完全发现不了。

4. **`camera_view_depth` 多了一个负号**。
   排序深度全部反号，不透明层变成"从远到近"绘制。画面**看起来正常**
   （不透明物体谁先画都对），但 early-z 完全失效，半透明顺序也错了。
   这是"不崩、不报错、只是变慢 + 偶尔不对"的典型。

5. **C 里不能对函数返回的结构体取地址**。
   `&(shader_desc_t)shader_builtin_lit_textured()` 既取不到地址，
   在 `-Wpedantic` 下也不合法。要先存成局部变量。

6. **`arena_allocator_t` 定义在 `arena_t` 之前** → 头文件类型顺序错误，编译失败。

7. **第三方单头库的告警要单独关**（`set_source_files_properties ... COMPILE_OPTIONS "-w"`）。
   注意 `COMPILE_OPTIONS` 是追加到目标选项之后的，所以 `-w` 能覆盖前面的 `-Wall`。

## 预判坑的命中情况

| # | 预判 | 结果 |
|---|---|---|
| 1 | 接口里漏出 GL 类型 | ✅ 全程守住了；示例里搜不到 `gl->` |
| 2 | "按 GL 直觉设计"的诱惑 | ✅ 用 `pipeline_desc_t` 收成一次性提交 |
| 3 | 顶点布局的表示 | ✅ binding 数组 + attribute 数组 + 语义枚举 |
| 4 | 纹理格式 | ✅ 引擎自定义 `pixel_format_t`，后端翻译 |
| 5 | sRGB | ✅ `texture_load_*` 带 `srgb` 参数，加载器不猜 |
| 6 | 句柄代际 | ✅ 两层句柄，代际校验在 renderer 层 |
| 7 | 排序稳定性 | ✅ 用 `sort_key + sequence` 构成全序，比稳定排序更省事 |
| 8 | 每帧临时数据 | ✅ 队列复用内存，追踪分配器断言零分配 |
| 9 | OBJ 加载 | ✅ 两遍扫描 + 三元组去重；1-based 与负索引都处理了 |
| 10 | 相机复用 M0 矩阵 | ✅ 内部调 `mat4_look_at` / `mat4_perspective` / `mat4_ortho` |
| 11 | 高 DPI 与视口 | ✅ 视口用物理像素（M1 已保证） |
| 12 | 最小化时 0 尺寸 | ✅ 示例里跳过绘制 |

---

## 本阶段目标

把 M1 那个"能在示例里画一个全屏三角形"的裸 GL 调用，变成引擎里可复用、
可组合、**且后端无关**的渲染核心：shader 封装、GPU 资源抽象、
纹理加载、相机系统、材质、渲染队列与排序，以及**渲染后端抽象接口**。

M1 结束时 `gl_api.h` 里那份"引擎用到的 GL 表面清单"就是本阶段接口设计的输入。

## 本阶段验收标准

* 能画三角形、贴图 quad、加载 OBJ 模型。
* 相机可切换正交 / 透视，且两者共用同一套投影约定（深度 [0,1]，见 M0 决策）。
* 资源用句柄（`handle_t`）而不是裸指针暴露给上层。
* 渲染后端接口 `render_backend_t` 用函数指针表实现，**按 Vulkan 语义定义**：
  显式 render pass、显式资源布局、不可变 pipeline state object、显式同步。
  GL 实现内部做"状态机降级适配"。
* 上层（游戏逻辑）不直接调用任何 GL 函数 —— 代码里搜不到 `gl->`。
* 帧内零 `malloc`：每帧的渲染队列、临时数据走 arena。
* 渲染层单元测试通过；示例 `02_render` 可跑。

## 本阶段技术约束

* 纯 C11，不引入新依赖（纹理加载用 stb_image，属于已批准的"单头库"）。
* `render_backend_t` 的接口一旦定下，后续 D3D11 / Vulkan 后端只做实现，
  不改接口。**这是本阶段最重要的约束。**
* 不提前实现 M3 的 2D 批渲染（精灵批处理属于 M3）。
* 不提前实现 M4 的 PBR / 阴影。

## 本阶段交付物

```
include/engine/render/
  render_backend.h       后端抽象接口（函数指针表，按 Vulkan 语义）
  render_types.h         后端无关的类型：纹理格式、混合模式、剔除模式、顶点布局
  texture.h              纹理（句柄制）
  shader.h               着色器程序（句柄制）
  mesh.h                 网格（顶点缓冲 + 索引缓冲 + 顶点布局）
  material.h             材质（shader + 参数 + 纹理绑定）
  camera.h               相机（正交 / 透视，产出 view / projection）
  render_queue.h         渲染队列与排序（按 layer + depth）
  renderer.h             渲染器（持有后端 + 资源池 + 队列）
src/render/
  backend/opengl/gl_backend.c      后端接口的 GL 实现
  renderer.c
  camera.c
  render_queue.c
  mesh.c / shader.c / texture.c / material.c
examples/02_render/main.c
tests/test_render.c
```

## 请帮我

1. 先给出 `render_types.h` + `render_backend.h` 的完整接口设计，
   **并逐条说明每个设计决策为什么是 Vulkan 友好的**。
2. 再给出 `camera.h`（正交/透视，复用 M0 的 `mat4_perspective` / `mat4_ortho`）。
3. 给出 GL 后端的实现骨架。
4. 给出 `02_render` 示例（三角形 + 贴图 quad + OBJ 模型 + 相机切换）。
5. 指出本阶段的坑。

## 不要

* 不要用 C++。
* 不要写伪代码。
* 不要引入 stb_image 之外的新依赖。
* 不要在接口里出现任何 GL 类型（`GLuint` / `GLenum` 等）。
  接口里出现 GL 类型就说明抽象漏了。
* 不要提前做 2D 批渲染（M3）和光照阴影（M4）。

---

## 我预判的坑（请逐条确认或纠正）

1. **接口里漏出 GL 类型**：最典型的失败模式。`render_backend_t` 里一旦出现
   `GLuint`，M13 接 D3D 时就得改接口。所有资源句柄必须是 `handle_t` 或
   自定义的 `texture_handle_t` 之类，映射到后端内部表。
2. **"按 GL 直觉设计"的诱惑**：GL 是"设状态 → 画"，接口很容易写成
   `set_blend_mode()` / `set_depth_test()` 这种散装状态设置。
   Vulkan 的等价物是 pipeline state object（一次性、不可变），
   所以接口应该收成 `render_pipeline_desc_t` 一次性提交。
3. **顶点布局的表示**：GL 用 `glVertexAttribPointer` 逐个描述，
   Vulkan 用 `VkVertexInputAttributeDescription` 数组。
   接口应该用后者那种"数组 + 语义枚举"的形式。
4. **纹理格式**：GL 的 internal format 有几十种，D3D/Vulkan 的枚举完全不同。
   接口只暴露引擎自己的 `pixel_format_t`（R8G8B8A8_UNORM、R8G8B8A8_SRGB、
   R8_UNORM、D32_FLOAT 等），后端负责翻译。
5. **sRGB**：不做决定的话，贴图颜色会明显偏暗或偏亮。
   需要明确：颜色纹理用 sRGB 格式 + `GL_FRAMEBUFFER_SRGB`，
  法线/粗糙度等数据纹理用 UNORM。这个决定要在 `pixel_format_t` 里体现。
6. **资源句柄的代际问题**：M0 的 `handle_t` 有 8 位代际。
   纹理/网格的句柄用同一套机制，卸载后旧句柄必须失效。
7. **渲染队列的排序稳定性**：同 layer 同 depth 的两个物体，排序必须稳定
   （用插入顺序做 tie-break），否则每帧的绘制顺序可能不同，
   表现为透明物体闪烁。
8. **每帧的临时数据**：渲染队列每帧重建，必须走 arena 而不是每帧 malloc/free。
9. **OBJ 加载**：OBJ 有 `.obj` / `.mtl` 两个文件，且索引是 1-based 的。
   另外 OBJ 没有统一的"单位"，加载后模型尺寸可能差几个数量级。
10. **相机与坐标约定**：M0 已经定死"右手系 / Y 上 / 看 -Z / 深度 [0,1]"。
    `camera.h` 必须复用 `mat4_look_at` 与 `mat4_perspective`，
    不要另写一份 —— 两份实现必然会在某个时刻不一致。
11. **`glViewport` 与高 DPI**：视口要用客户区的**物理像素**尺寸，
    不是逻辑尺寸。M1 已经保证 `platform_window_get_size` 返回物理像素。
12. **最小化时的 0 尺寸**：客户区宽或高为 0 时 `glViewport(0,0,0,0)` 是合法的
    但会导致后续绘制被丢弃；M1 示例里已经跳过这种情况，渲染核心也要处理。

## 本阶段的"完成"定义

示例程序 `02_render` 能：
* 用引擎接口（不直接调 GL）画出一个贴图 quad 和一个 OBJ 模型；
* 相机可在正交 / 透视之间切换，模型可旋转缩放；
* 控制台显示 draw call 数、三角面数、帧时间；
* 按 ESC 退出，退出时零资源泄漏、零 GL 错误；
* `cmake --build --preset werror` 零告警；
* 单元测试覆盖相机矩阵、渲染队列排序、句柄生命周期。
