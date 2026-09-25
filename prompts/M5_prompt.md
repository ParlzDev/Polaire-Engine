# 当前阶段：M5 —— ECS + 场景

**状态：已完成（2026-09-25）**

> 交付情况与踩坑记录见 `docs/M5-设计决策.md`。
> 两处与计划的偏离，都是有意的：
> ① `gltf_loader` / `.mesh` 相关不动（M4 已交付），本阶段不碰资源加载。
> ② "1000 个 sprites 的示例"改成 1k / 10k / 100k 三档实测：
>    只测 1000 个的话，任何 O(n²) 都能混过去 —— 本阶段两个真实 bug 之一正是这么藏住的。
>
> 收口时补上的两件事（原先记成"偏离"，其实计划要求得很清楚，是我读漏了）：
> 一是 **JSON 存档**（验收原文："二进制与 JSON 两条路往返一致"、交付物里有
> `assets/scenes/m5_demo.scene(.json)`），做法是二进制的**转码**，见决策文档第十一节；
> 二是 **2D/3D 同帧共存与含 present 的整帧帧率**做成了程序自判、退出码说话的验证
> （`--check-pixel`、bench 的 `[D]` 相位），顺带查出并修掉一个真实缺陷：
> 世界矩阵的求值挂在"有没有系统注册在 TRANSFORM 阶段"上（决策文档第十二节）。

> 用法：粘贴 `AI_PROMPT.md` 全文 + 本文件全文给 AI。

---

## 本阶段目标

把"每帧手写一遍场景"变成"描述场景 + 跑系统"。M5 结束后，
`06_scene` 里不应该有 for 循环画物体，只有：注册组件类型、建实体、加系统、跑。

分层目标里的位置：M5 是 ECS 层，往下接 M2-M4 的渲染核心，往上承 M6 的 2.5D 混合与
M11 的编辑器。**M5 的接口一旦定歪，M6/M11 全在偿贷**，所以这一阶段的
设计评审比实现更重要。

## 本阶段验收标准

* 纯数据驱动场景：场景内容全部来自文件，代码里不出现具体物体清单。
* 2D / 3D 实体共存于同一世界、同一队列（靠 render layer 区分，不靠两套架构）。
* 1000 实体场景稳定 60 FPS —— **并且要实测 1 万 / 10 万**，
  只验 1000 等于没验（这个规模下任何实现都能过）。
* 场景可存可读：二进制与 JSON 两条路往返一致（存 → 读 → 再存，字节相同）。
* 父子关系：移动父节点，子节点的世界变换跟着变；场景里有 3 层以上链条。
* Prefab：把一个实体子树存成模板，在另一处实例化 100 次，改动模板不影响已实例化的。
* 上层不直接调 GL 这条**继续成立**；ECS 模块本身**不知道 renderer 的存在**（见约束 3）。
* 单元测试：句柄代际、组件池增删、query 位图、父子环检测、
  世界变换求值顺序、序列化往返、prefab 实例化计数。

## 本阶段技术约束

1. 纯 C11，不引新依赖。JSON 解析器自己写（`core/json.h`），
   因为 M7 的 GLTF 也要用它 —— 现在写一次，M7 白拿。
2. **复用 M0 的 `pool_t`，不改它的签名。** 它的两个既有特性直接决定 ECS 形状，
   必须正面处理而不是绕过：
   * `pool` **不扩容**（扩容会移动 data 指针使裸指针失效）
     → 每个组件类型必须在注册时给出容量上限，需要一套"容量从哪来"的答案（见决策 D2）。
   * 代际只有 **8 位**（要和 `handle_t` 的打包宽度对齐）
     → 同一槽位复用 256 次后代际回绕，旧句柄会**重新变成有效**并指向新对象。
3. **依赖方向**：`ecs` 层不 include 任何 render 头文件。
   桥接放在 `scene/`（或 `render/ecs_submit.c`）：它遍历 query，把结果喂给
   `renderer_push_mesh` / `sprite_batch_draw`。这样 ECS 能被编辑器、
   物理、脚本复用，也避免 ECS 与 GL 后端耦合死。
4. 不提前做 M6 的双相机/共享深度、M7 的异步加载与世代句柄、M12 的脚本挂载点。
   （但 M5 要**给 M12 留一个不破坏封装的位置**：组件的函数指针表里
   只放数据描述与两个生命周期钩子，不放任意回调，见 D3。）
5. 帧内零 malloc 继续成立：世界级结构用 `pool_t`（定容）+ `array_t`
   （会扩容但**稳态后不再扩**，`array_clear` 不释放内存），
   帧内临时数据走 `renderer_frame_arena`。

## 关键设计决策（每条给了备选与推荐）

### D1 组件存储：per-type SoA + 存在性位图

| 方案 | 迭代局部性 | 复杂度 | 结论 |
|---|---|---|---|
| **A. 每组件类型一个 pool + 分块位图** | 单类型遍历完美；多类型 join 靠位图探测 | 低 | **推荐** |
| B. Archetype/表存储 | join 也连续 | 高（archetype 图、增删组件要搬实体） | 不做，留升级路径 |
| C. 每实体一个组件数组（ AoS-ish） | 差，指针追逐 | 低 | 否决 |

`view<A,B>` 的驱动策略：**用基数最小的 all 集合作为驱动池**，
其余集合用位图 O(1) 探测。否则"1 万个 Transform 里只有 3 个 Sprite"
这种查询会白扫 1 万次。位图按 64 位字分块，`__popcnt`/`tzcnt` 可直接跳过空块。

升级触发线（写死在文档里，避免将来凭感觉改架构）：
当"多类型 join"的实测时间超过单帧预算 5% 时，才考虑往 B 走。

### D2 组件注册与容量

组件类型**运行时注册**，不是编译期枚举 —— 加组件不用改 ECS 核心
（M11 编辑器、M12 脚本都要在不动 ECS 的前提下加组件）。

```c
typedef struct component_desc_t {
    const char          *name;      // 存档存名字，不存 id
    u32                  size;
    u32                  align;
    u32                  capacity;  // pool 不扩容，这里给上限
    const ecs_field_desc_t *fields; // 见 D3
    u32                  field_count;
    void (*construct)(void *component);              // 新实体的默认值
    void (*post_load)(void *component, struct ecs_world_t *world); // 句柄修复
} component_desc_t;
```

`capacity` 从哪来 —— 这是 per-type pool 的真实痛点（10 万实体 × 5 个组件
= 5 份独立容量估算）。提案：世界初始化时给一个**默认容量**，注册时可覆盖；
每次分配失败都记 WARN 并统计"还差多少"，跑一遍场景就知道该配多大。
比拍脑袋预分配更诚实。**不做自动扩容**（会移动 data，毁掉裸指针）。

### D3 序列化靠"字段描述表"，不靠手写每类型的读写

这是 M5 里杠杆最大的一处，也是 M11 属性面板的地基。

```c
typedef enum ecs_field_kind_t {
    ECS_FIELD_F32, ECS_FIELD_U32, ECS_FIELD_I32, ECS_FIELD_BOOL, ECS_FIELD_ENUM,
    ECS_FIELD_VEC2, ECS_FIELD_VEC3, ECS_FIELD_VEC4, ECS_FIELD_QUAT,
    ECS_FIELD_ENTITY, // handle；序列化写 dense id，读回时重映射
    ECS_FIELD_STRING, // 内联 char[N]，带 capacity
    ECS_FIELD_STRUCT, // 内嵌已注册的小结构（如 transform 的三个成员）
} ecs_field_kind_t;

typedef struct ecs_field_desc_t {
    const char      *name;
    u16              offset;
    ecs_field_kind_t kind;
    u16              count;    // 数组长度，1 = 标量
    u16              capacity; // STRING 的字节容量
    u16              reserved;
    const char      *struct_type;
} ecs_field_desc_t;
```

有了它，二进制写/读、JSON 写/读、编辑器的属性面板**都是同一份代码**，
不需要每加一个组件就手写 4 个序列化函数。

方案对比：

| 方案 | 加组件的成本 | 版本兼容 | 结论 |
|---|---|---|---|
| A. 字段描述表（反射） | 只写一张表 | 按 name 匹配，改结构不炸 | **推荐** |
| B. 每类型注册 save/load 回调 | 每类型手写两份，且容易漏 | 手动 | 只在字段表表达不了时才用 |
| C. 只支持内置固定几类组件 | 0 | 无 | 编辑器与 prefab 会立刻撞墙 |

代价说清楚：C 里没有真正的反射，字段表要**手维护**，会与结构体漂移。
防线是 `_Static_assert` + 单测：对每个内置组件断言
`offsetof(字段)` 与表里写死的 offset 一致（引擎里已有 170 处这种断言的先例，
`object_uniforms_t` 就是这么防 std140 漂移的）。

### D4 世界变换：拍平拓扑序，不用递归

子节点世界变换 = 父世界 × 本地。求值顺序错了就慢一帧（表现为"抖动"）。

| 方案 | 说明 | 结论 |
|---|---|---|
| A. 递归求值 + memo | 代码最短 | 否决：**递归深度不可控就是栈溢出** |
| B. 维护拍平的拓扑序数组，层级变化时重建 | 每帧顺序扫一遍，父先于子 | **推荐** |
| C. 每帧全量重算所有实体 | 最简单 | 1 万实体时浪费明显，但可作为 B 的兜底验证 |

B 的层级变化用脏标记延后重建（一帧最多一次），重建时检测：
* **父链成环** → 拒绝并报错（`set_parent` 时先上溯一遍，带深度上限）
* **深度超上限**（如 64）→ 拒绝，防编辑器里手滑拖成无限层

### D5 系统调度：固定 stage + 全序 + 延迟的结构变更

```c
typedef enum ecs_stage_t {
    ECS_STAGE_INPUT, ECS_STAGE_LOGIC, ECS_STAGE_MOVE,
    ECS_STAGE_RELATION,   // 父子/约束：世界变换在此之前
    ECS_STAGE_TRANSFORM,  // 算世界矩阵
    ECS_STAGE_RENDER_SUBMIT,
    ECS_STAGE_CLEANUP,    // 处理销毁与结构变更队列
    ECS_STAGE_COUNT
} ecs_stage_t;
```

同一 stage 内按 `priority`，再按注册名，构成**全序** —— 和 M2
`render_queue` 的确定性排序是同一个原则：**顺序不确定的引擎没法复现 bug**。

**迭代期间的结构性修改**（加/删组件、销毁实体）是 ECS 的头号坑：
会移动 pool 槽位、翻转位图，直接毁掉正在跑的迭代器。
提案：**命令队列**，stage 边界统一 flush。API 上不做"立即版"，
只留一个 `ecs_defer_*` 家族 —— 因为一旦允许立即版，
就会出现"在这个回调里安全、在那个里不安全"这种没人记得住的规则。

### D6 句柄与实体 id

`entity_t = handle_t`（24 位 index + 8 位 generation），**直接复用 M0 的池句柄**。

两个必须正面处理的点：

1. **8 位代际回绕**。空闲链表若是 LIFO，一个热点槽位可能在几秒内复用
   256 次，之后旧句柄重新"有效" —— 表现是"早就销毁的敌人突然又能读到"。
   提案：把组件/实体 pool 的空闲链表按 **FIFO 轮转**发放，
   让回绕周期从 256 次放大到 `256 × 槽位数`。这个改动在 `pool_t` 内部，
   **不改任何对外签名**。
2. **句柄不能直接进存档**。存档里的 index 在重新加载后属于另一个实体。
   提案：序列化写 **dense id**（场景内的紧凑编号），
   加载时建"旧 dense id → 新 entity_t"映射表，`post_load` 钩子里重映射。
   指向场景外（如资源句柄）的引用在 prefab 里一律记为无效。

## 交付物

```
include/engine/core/json.h          JSON 词法/语法（自己写，M7 的 GLTF 复用）
include/engine/core/byte_io.h       小端字节读写（mesh_file.c 现在自己实现了一份，收编进来）
include/engine/ecs/entity.h         实体、组件注册、增删查
include/engine/ecs/component.h      字段描述表、内建组件（transform/sprite/mesh/material/camera/light/tag）
include/engine/ecs/query.h          query 描述、位图、行游标
include/engine/ecs/system.h         系统注册与调度、命令队列
include/engine/ecs/world.h          世界生命周期、每帧驱动、统计
include/engine/scene/scene.h        场景 = 世界 + 层/相机/光照等环境
include/engine/scene/scene_io.h     二进制 + JSON 存读
include/engine/scene/prefab.h       模板与实例化
include/engine/scene/ecs_submit.h   ECS → render_queue 的桥（唯一同时看得见两边的位置）
src/core/json.c  src/core/byte_io.c  src/ecs/*.c  src/scene/*.c
examples/06_scene/main.c            数据驱动的 2D/3D 混合场景
tests/test_ecs.c                    句柄/池/query/父子/环/深度上限
tests/test_ecs_serialize.c          字段表往返、dense id 重映射、schema 漂移检测
tests/test_prefab.c  tests/test_json.c
assets/scenes/m5_demo.scene(.json)  同一场景的两种格式
```

`06_scene` 的硬性要求：**代码里不出现实体清单**。全部来自
`assets/scenes/m5_demo.scene*`。命令行支持切换两种格式，
用来证明"存读的不是同一种东西的两份拷贝"。

## 我预判的坑（请逐条确认或纠正）

1. **迭代时改结构**是最大的一个。表现为"偶发崩溃"和"某个实体被悄悄改坏"，
   而不是干净的错误。必须一开始就上命令队列，别等出问题再补。
2. **组件里放裸指针**。省事的写法，但序列化会静默丢掉它、复制 prefab 会复制出野指针。
   规则：组件只允许 POD + 句柄。要引用别的东西就用 handle/dense id。
3. **字段表与结构体漂移**。C 没有真反射，表是手写的，改字段忘了改表 →
   存档错位且不报错。必须用 `offsetof` + `_Static_assert` 钉死，
   并且**存档头写 schema 哈希**，读到时不匹配就明确拒绝，
   而不是"能读出来一半"。
4. **per-type pool 的容量**会被反复"猜错→崩→加大"。
   把"分配失败次数 + 峰值占用"做成世界统计项，让容量是量出来的不是猜的。
   注意"注册顺序决定 type id"这个隐式全局状态：
   如果两个模块都注册同名组件，必须报错而不是发两个 id。
5. **父子环**在编辑器手滑时会真的出现（把父拖到自己孩子下面）。
   `set_parent` 里上溯检测 + 深度上限，别指望"运行时不会有人这么干"。
6. **世界变换不要用递归**。今天刚在同一个仓库里查过一个栈踩踏的问题
   （见 docs/M4-设计决策.md 第二节），递归深度不可控就是同一个雷区。
7. **渲染合批与 ECS 迭代顺序冲突**。ECS 的迭代顺序是"内存布局顺序"，
   而精灵批渲染要求同纹理的连续提交。**不要**直接从 query 里调 `renderer_push_*`，
   一律经 `render_queue` 排序，否则帧率会莫名其妙抖。
8. **2D 与 3D 组件的 transform 语义不同**（2D 常用像素、z 是层内排序键；
   3D 用世界单位）。不要用两套 transform —— 那等于 M6 再翻一次。
   用同一份 transform，差异体现在提交时选哪个 layer / 是否投影视图。
9. **`view<A,B>` 驱动池选错**会从 O(命中的实体数) 退化成 O(场景全部实体)。
   在 query 构造时就按基数挑最小的池，并把这个选择打进调试信息。
10. **序列化"能读回来"不等于"读对了"**。判据必须是往返三方对比：
    原场景 → 存 A → 读 → 存 B，要求 A 与 B **字节相同**；
    再检查 A 能读回。只做"存了能读"会漏掉浮点与句柄的重映射问题。
11. **float 进存档**用定点或十六进制文本都会有精度争议。
    二进制走小端 f32 位模式（无损）；JSON 走 `%.9g`（能精确往返），
    并要求"存了读、读了再存"逐字节一致 —— 让浮点格式自己证明可逆。
12. **JSON 解析器的边界**（转义 `\u`、嵌套深度、数字格式）自己写很容易只覆盖
    "我手写的样例"。要准备一批负面用例：截断、超深嵌套、非法转义、
    重复 key、`1e400` 这类越界数字。解析器错误必须拒绝而非部分成功。

## 不要

* 不要用 C++ 思路硬套（模板 view、RAII 析构）。
* 不要引 EnFFt/Entt/flecs 之类第三方 ECS。
* 不要为了"ECS 正统"把已经验证过的 `render_queue`/`sprite_batch` 重构掉 ——
  M5 是**加一层**，不是拆地基。M2-M4 的接口签名不动。
* 不要在 ECS 里 include GL/render 头（见约束 3）。
* 不要做 archetype、多线程并行调度、脚本回调 —— 都有明确的更晚的主。
* 不要把 `ecs_world_t` 做成全局单例（引擎总目标是可多实例）。

## 本阶段的"完成"定义

* `06_scene` 从一个数据文件建出 2D + 3D 混合场景并稳定 60 FPS，
  代码里看不到物体清单。
* 实测并记录：1k / 10k / 100k 实体的帧时间与内存占用（含"移动系统 + 提交系统"）。
* 同一场景的二进制与 JSON 往返逐字节一致；两种格式渲染结果一致。
* Prefab 实例化 100 次后销毁全部实例，池占用回到实例化之前（无泄漏、无碎片错觉）。
* 父子 3 层链：动父节点，子节点世界位置正确；构造环被拒绝并给出可读错误。
* `cmake --build --preset werror` 零告警；ASan+UBSan 下跑完全部新测试无报错。
* 退出零内存泄漏、零 GL 错误。
* `docs/M5-设计决策.md` 记录定稿结论，包括 D1-D6 最终选了哪个、为什么。
