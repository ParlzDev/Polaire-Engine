// =============================================================================
//  engine/scene/scene_io.h —— 场景存读（二进制 + JSON 两种编码）
//
//  ---- 一套遍历服务三处数据 ----
//
//  环境块、资源条目、组件全都是"一块内存 + 一张字段描述表"的形状，
//  所以只有一套编解码。手写第二份字段名清单必然漂移 ——
//  环境段我上一版就是这么写的，结果只有部分字段能落盘。
//
//  ---- 不整块 memcpy ----
//
//  把结构体原样写进文件，等于把编译器的填充规则也写进了格式：
//  换编译器、加字段、调成员顺序，存档就会**静默错位**，
//  症状是"读出一堆乱值且不报错"。所以按表逐字段读写，
//  padding 一个字节都不落地（vec3_t 内存里 16 字节含 pad，文件里只有 12 字节）。
//
//  由此得到一条有用的性质：**每种结构的字节尺寸可以纯从表算出来**。
//  于是能给每个组件写上精确长度，读侧遇到不认识的类型时整块跳过继续读，
//  而不是放弃整个场景。
//
//  ---- 文件布局 ----
//
//      [0,64)     头部（魔数、版本、注册表哈希、各段计数、存档种类、根 dense）
//      环境块 → 类型表 → meshes → atlases → materials
//      实体索引段 → 实体数据段
//
//  段计数一律在头部，**不在各段自己的开头** —— 两处都写就要对账，
//  而对账的结果通常是"其中一处是过期的"。类型表和两个实体段例外：
//  它们在段首重复了一次计数，读侧拿它当"我还在正确的偏移上"的哨兵，
//  这三次重复换来的是四处提前失败而不是四处静默错位。
//
//  实体拆成"索引 + 数据"两段，是为了**一遍线性读**就能完成 dense id 重映射：
//  索引段先把所有实体建出来并填好映射表，数据段再翻译父子与组件内的实体引用。
//  否则就要"读两遍同一段"，那需要给字节流加 seek ——
//  多一个 API、多一处能写错的地方，而收益只是省 12 字节每实体。
//
//  ---- 资源引用落名字，实体引用落 dense id ----
//
//  句柄和表下标都是"这一次运行"的产物。落索引的话，
//  在资源列表中间插一行就让后面全体错位，而且**读得出来** ——
//  只是"这个模型穿了那个模型的皮"。名字让人手编辑仍然安全。
//
//  ---- 四处必须失败或明确报告，不能将就 ----
//
//  1. 注册表哈希不符 → 拒绝整份存档。旧存档迁移是 M11 的活，
//     届时是显式的版本转换器，不是"猜着读"。
//  2. 类型名查不到 → 整块跳过该组件（按类型计数，不按实例），其余照常
//     （一个组件不毁掉整个场景）。
//  3. 资源/实体引用翻译不到 → 照样建实体，字段留空并计数。
//     表现是"某个物体没材质"，比"整个场景加载失败"好归因。
//  4. 布尔读到 0/1 以外 → 判格式错位。静默放宽会把格式问题伪装成数据问题。
//
//  ---- 验收判据 ----
//
//  "存 → 读 → 再存"两次输出**逐字节相同**。这条比"能读回来"强得多：
//  它同时验了写出顺序稳定（按 dense id、按类型 id）、浮点可往返、
//  dense id 重映射没接错、资源名字翻译没偏移。见 tests/test_scene_io.c。
//
//  一个必须说清的限定：**加载会把 dense 空间压实**。实体销毁后 id 不回收，
//  所以编辑器里存出的第一份存档 dense 列是带洞的（0,1,2,4,6），
//  读进来再存就变成 0,1,2,3,4 —— 这两次字节可以不同（内容相同）。
//  从第二次起进入规范形，往后再怎么往返都逐字节相同。
//  所以判据准确地说是"任意场景读一遍之后，往返逐字节一致"，
//  而 tests 里那条 dense_holes 就是钉这个的。
//
//  ---- 两种编码：二进制与 JSON ----
//
// 同一份存档有两种形式。**JSON 不是第二套序列化器**：它是二进制存档的转码
// （见 src/scene/scene_json.c 的文件头）。为什么：
//
// * "哪些实体进存档、按什么顺序、引用子树外的实体怎么算、预制体带哪几条资源"
//   这些问题各自有答案，而两份实现会让它们在某个时刻分叉 —— 本阶段查出的
//   格式 bug 全部来自"同一个量算两遍、两遍不一样"这一个形状。
// * 转码之所以可能，是因为二进制格式自己就是自描述的：资源落名字、
//   实体引用落 dense id、记录长度可纯从字段表算出。
// * 于是校验只有 scene_io_read_archive 一处，两种编码共用同一个守门人。
//
// 两种形式的取舍：二进制是引擎默认用的（小、快、什么值都存得下）；
// JSON 是给人看、给人手改的（可以只写关心的那几个字段，省略的取默认值：
// 组件走 construct、环境走 scene_env_default()、材质走 material_default()）。
// 一处必须的差别：JSON 没有 NaN/Infinity 的字面量，所以**含非有限浮点的场景
// 存不成 JSON**（会明确报错而不是产出非法文档），二进制不受这条限制。
// =============================================================================
#ifndef ENGINE_SCENE_SCENE_IO_H
#define ENGINE_SCENE_SCENE_IO_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/scene/scene.h"

#define SCENE_FILE_MAGIC 0x4E454353u // 按字节写出去是 'S','C','E','N'
#define SCENE_FILE_VERSION 1u
#define SCENE_FILE_HEADER_SIZE 64u

// 存档种类写在头部第 9 个 u32 上。为什么不用"root_dense != NONE"推断：
// 那种隐含的判别法在两年后回头看会变成"某个字段有双重含义"，
// 而双重含义的字段是格式演进时最先腐烂的地方。
typedef enum scene_archive_kind_t {
    SCENE_ARCHIVE_SCENE = 0u,  // 整场景：环境块是数据
    SCENE_ARCHIVE_PREFAB = 1u, // 子树：环境块按默认值写、读侧不应用
    SCENE_ARCHIVE_KIND_COUNT
} scene_archive_kind_t;

// 头部布局（每个字段一个 u32，小端；下标是"第几个 u32"，字节偏移 = 下标 × 4）：
//   [0] 魔数  [1] 版本  [2] 注册表哈希  [3] 实体数  [4] 网格数  [5] 图集数
//   [6] 材质数  [7] 类型数  [8] 存档种类  [9] 根实体的 dense id  [10,16) 保留
#define SCENE_FILE_OFF_ENT 12u
#define SCENE_FILE_OFF_MESH 16u
#define SCENE_FILE_OFF_KIND 32u
#define SCENE_FILE_OFF_ROOT 36u

typedef struct scene_io_stats_t {
    u32 entities_written;
    u32 entities_loaded;
    u32 components_written;
    u32 components_loaded;
    u32 resources_loaded;

    // 三类"部分失败"必须让调用方看得见，否则场景会静默变样。
    // 前两个的单位是**类型**而不是实例：一个类型不认识，丢掉的实例数随场景而变，
    // 报成实例数会让人以为"就坏了 37 个"，而真正该做的事是去补上那个类型。
    u32 unknown_component_types;
    u32 schema_mismatches;
    // 引用翻译不到（资源名不在表里、实体 dense id 不存在）。这个确实是按**字段**数，
    // 因为它指向的是一个个具体的引用，修法是改那条引用。
    u32 dangling_refs;
} scene_io_stats_t;

// ---------------------------------------------------------------- 内存形式
//
// 单元测试走这两个：不碰文件系统也不碰窗口（scene 用离线模式建）。
//
// 写函数也取非 const scene_t：读组件要过 ecs_peek(const world)，
// 而 scene_world() 这类访问器本身就要求非 const 场景。写成 const 只会在
// 实现里逼出一次剥 const 的强转 —— 诚实的类型比好看的类型有用。
//
// out_stats 可以为 NULL。写侧也报 dangling_refs 是有意为之：
// 组件里存着一个越界的资源下标，**在存盘那一刻就已经是 bug 了**，
// 等到读盘才报（而且读侧看到的是一个空名字，连"曾经有过一个坏引用"都不知道），
// 就等于把案发现场擦掉再报警。
bool scene_write_binary(scene_t *scene, arena_t *arena, void **out_bytes, usize *out_size,
                        scene_io_stats_t *out_stats);
bool scene_read_binary(scene_t *scene, const void *bytes, usize size, scene_io_stats_t *out_stats);

// 带"根实体"输出的读档，预制体实例化走这条。
// 整场景存档没有根（头部那个字段是 NONE），此时 *out_root = ENTITY_NONE。
bool scene_read_binary_root(scene_t *scene, const void *bytes, usize size,
                            scene_io_stats_t *out_stats, entity_t *out_root);

// ---------------------------------------------------------------- 预制体形式
//
// 预制体就是"一份带根的场景存档"，所以它和场景共用同一套编解码 ——
// 不是图省事：两份实现会在同一天以两种不同的方式烂掉，而这份格式的
// 每一次改动都必须同时体现在存和读两侧。
//
// 和整场景存档的三点差别：
//   1. 只写 root 及其后代，dense id 在子树内重新从 0 编号；
//      引用子树外的实体一律变 NONE（预制体不许偷偷依赖外面的东西）；
//   2. 只写**被引用到**的资源，不是源场景的整张资源表。
//      否则"实例化一个炮塔"会把 200 个网格名一并灌进目标场景，
//      资源表上限 128 —— 第三次实例化就直接失败了；
//   3. 环境块按默认值写出、读侧不应用：预制体没有"环境"这个概念，
//      但段还在，这样格式只有一种、偏移只有一套。
bool scene_write_prefab(scene_t *scene, entity_t root, arena_t *arena, void **out_bytes,
                        usize *out_size, scene_io_stats_t *out_stats);
bool scene_save_prefab(scene_t *scene, entity_t root, const char *path,
                       scene_io_stats_t *out_stats);

// 子树大小（含 root 自己）。0 = root 无效。06_scene 与测试用它核对实例数。
u32 scene_subtree_size(scene_t *scene, entity_t root);

// ---------------------------------------------------------------- 文件形式
//
// 加载**不清空世界**，只追加。不给 auto_clear 开关：
// "读个文件顺手清掉一半状态"这种隐式副作用在调用点上看不出来，
// 而它的反面（先 ecs_world_clear 再 load）是两行一目了然的代码。
bool scene_save_binary(scene_t *scene, const char *path, scene_io_stats_t *out_stats);
bool scene_load_binary(scene_t *scene, const char *path, scene_io_stats_t *out_stats);

// ---------------------------------------------------------------- JSON 形式
//
// 与上面六个函数**语义完全等价**，只是编码不同（实现是转码，理由见文件头）。
// 名字成对：scene_write_json ↔ scene_write_binary，如此类推。
//
// out_text/out_length 给出 JSON 文本；缓冲区末尾会多写一个 NUL，
// 所以可以直接当 C 字符串打印与断言，但 *out_length 不含它。
// 文本分配在调用方给的 arena 上（save/load 两个入口自己走 scratch 并丢掉）。
//
// 手写的文件可以只写关心的字段：环境、三段资源、实体四段都是可选的，
// 记录里没写的字段取默认值。但**形状**必须对：向量分量要齐、顶层不许有认不出的键、
// 一个实体里同一组件不能出现两次 —— 这类错误会整份拒绝并点名，见 scene_json.c。
bool scene_write_json(scene_t *scene, arena_t *arena, void **out_text, usize *out_length,
                      scene_io_stats_t *out_stats);
bool scene_read_json(scene_t *scene, const char *text, usize length, scene_io_stats_t *out_stats);
bool scene_read_json_root(scene_t *scene, const char *text, usize length,
                          scene_io_stats_t *out_stats, entity_t *out_root);
bool scene_write_prefab_json(scene_t *scene, entity_t root, arena_t *arena, void **out_text,
                             usize *out_length, scene_io_stats_t *out_stats);
bool scene_save_json(scene_t *scene, const char *path, scene_io_stats_t *out_stats);
bool scene_save_prefab_json(scene_t *scene, entity_t root, const char *path,
                            scene_io_stats_t *out_stats);
bool scene_load_json(scene_t *scene, const char *path, scene_io_stats_t *out_stats);

// ---- 两种编码互转（"两者可互相转换"这条判据的显式形状）----
//
// 都不读进场景、也不改场景状态：只借场景的**组件注册表**去解释类型表
// （没有字段表就读不出记录，两种方向都一样）。
// 为什么把转码公开出来而不是让调用方走"读进来再写出去"：
// 后者会顺手做一遍 dense 压实与资源表重建，于是"转换有没有丢内容"
// 这件该单独问的事，被混进"加载会不会规范化"里问不出来了。
// 逐字节比对这两条的结果，才是"两种编码描述同一份数据"的直接证据。
bool scene_json_from_binary(scene_t *scene, const void *bytes, usize size, arena_t *arena,
                            void **out_text, usize *out_length);
bool scene_binary_from_json(scene_t *scene, const char *text, usize length, arena_t *arena,
                            void **out_bytes, usize *out_size);

// 按文件扩展名挑格式：".json"（大小写不敏感）走 JSON，其余走二进制。
// 为什么放引擎里而不是让每个调用方自己 strcmp 后缀：06_scene、编辑器、
// 热重载 watcher 都要回答"这个文件是什么编码"，而答错的症状是
// "二进制存档被当 JSON 解析，报一个看不懂的语法错误"。
// 存盘没有 *_auto：路径是调用方决定的，"同一个路径下一次是二进制下一次是 JSON"
// 正是最难归因的一类 bug，所以存哪一种是显式调用哪个函数的事。
bool scene_path_is_json(const char *path);
bool scene_load_auto(scene_t *scene, const char *path, scene_io_stats_t *out_stats);

// 整张注册表的合并 schema 哈希，写进文件头。
// 一次比较就回答"这份存档是不是这个版本的引擎能读的"。
u32 scene_registry_hash(scene_t *scene);

// 只 peek 头部的两个计数，**不做任何校验**（校验在 scene_read_binary* 里，只有一处）。
// 之所以放在这里而不是让 prefab.c 自己去按偏移取字节：头部的布局只有这一个文件
// 应该知道 —— 把偏移抄进第二个 TU，就是给格式埋第二处会漂的地方。
bool scene_archive_peek(const void *bytes, usize size, u32 *out_entities, u32 *out_kind);

#endif // ENGINE_SCENE_SCENE_IO_H
