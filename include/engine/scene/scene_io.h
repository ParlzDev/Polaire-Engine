// =============================================================================
//  engine/scene/scene_io.h —— 场景存读（二进制）
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
//      [0,64)     头部（魔数、版本、注册表哈希、各段计数）
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
//  JSON 形式挪到 M11（编辑器/工具链）：那时它有明确的使用者，
//  而且只需给这套遍历再加一个 sink，不必在 M5 同时验证两条输出路径。
// =============================================================================
#ifndef ENGINE_SCENE_SCENE_IO_H
#define ENGINE_SCENE_SCENE_IO_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/scene/scene.h"

#define SCENE_FILE_MAGIC 0x4E454353u // 按字节写出去是 'S','C','E','N'
#define SCENE_FILE_VERSION 1u
#define SCENE_FILE_HEADER_SIZE 64u

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

// ---------------------------------------------------------------- 文件形式
//
// 加载**不清空世界**，只追加。不给 auto_clear 开关：
// "读个文件顺手清掉一半状态"这种隐式副作用在调用点上看不出来，
// 而它的反面（先 ecs_world_clear 再 load）是两行一目了然的代码。
bool scene_save_binary(scene_t *scene, const char *path, scene_io_stats_t *out_stats);
bool scene_load_binary(scene_t *scene, const char *path, scene_io_stats_t *out_stats);

// 整张注册表的合并 schema 哈希，写进文件头。
// 一次比较就回答"这份存档是不是这个版本的引擎能读的"。
u32 scene_registry_hash(scene_t *scene);

#endif // ENGINE_SCENE_SCENE_IO_H
