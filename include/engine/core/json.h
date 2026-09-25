// =============================================================================
//  engine/core/json.h —— 自己写的 JSON 解析 / 生成
//
//  ---- 为什么 M5 就要它 ----
//
//  M5 的场景序列化要求"二进制 + JSON 两种格式都能存读"，JSON 那份给人看、
//  给编辑器与手工调参用。M7 的 GLTF 加载更是**必须**先有一个 JSON 解析器
//  （GLTF 的 .gltf 就是 JSON + buffer 引用）。现在写一次，M7 直接用。
//
//  ---- 设计选择 ----
//
//  * **DOM + arena**。解析结果全部分配在传入的 arena 上，释放 arena 即全部回收，
//    没有"谁负责 free 哪个节点"的问题。场景文件几百 KB，DOM 的开销可以接受。
//    不用 SAX（事件流）：那会让"读一个嵌套字段"变成调用方自己写状态机。
//  * **字符串一律解码进 arena**，不做"无转义就零拷贝指向源文本"的快速路径。
//    零拷贝会让字符串有两种生命周期来源，而这两种在调试器里长得一模一样。
//  * **错误必须拒绝，不能部分成功**。解析失败时 out_root 保持 NULL，
//    并给出错误消息与字节偏移 —— "能读出一半"是最糟的结果，
//    它会让人以为文件没坏。
//
//  ---- 刻意收紧的地方（都是负面用例，见 tests/test_json.c）----
//
//  * 嵌套深度上限：递归下降解析器遇到 10 万层 `[` 会栈溢出。
//  * 拒绝重复 key：`{"a":1,"a":2}` 静默取后者会掩盖编辑器的 bug。
//  * 拒绝尾部垃圾：`{} {}` 不是合法 JSON 文档。
//  * 拒绝 NaN / Infinity：不是 JSON。
//  * 数字用 strtod 系列手工判定边界，不接受 `1e400` 这种溢出成 inf 的值。
// =============================================================================
#ifndef ENGINE_CORE_JSON_H
#define ENGINE_CORE_JSON_H

#include "engine/core/byte_io.h"
#include "engine/core/memory.h"
#include "engine/core/types.h"

#define JSON_MAX_DEPTH 64u

typedef enum json_kind_t {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
    JSON_KIND_COUNT
} json_kind_t;

typedef struct json_value_t {
    json_kind_t kind;

    f64        number; // JSON_NUMBER
    bool       boolean; // JSON_BOOL
    const char *string; // JSON_STRING：UTF-8，NUL 结尾
    usize      string_length; // 不含结尾 NUL

    // JSON_ARRAY：items[0..item_count)
    // JSON_OBJECT：keys[i] 与 items[i] 一一配对（keys 按原始顺序，便于回写时保持一致）
    struct json_value_t **items;
    usize                 item_count;
    const char          **keys;
} json_value_t;

const char *json_kind_name(json_kind_t kind);

// ---------------------------------------------------------------- 解析
//
// 成功返回 true 且 *out_root 非空。失败返回 false，并把原因写进 out_error
// （静态字符串，不必释放）与 out_error_offset（字节位置）。
// out_error / out_error_offset 允许为 NULL。
bool json_parse(arena_t *arena, const char *text, usize length, const json_value_t **out_root,
                const char **out_error, usize *out_error_offset);

// 便捷版：整个缓冲区当以 NUL 结尾的字符串解析。
bool json_parse_cstring(arena_t *arena, const char *text, const json_value_t **out_root,
                        const char **out_error, usize *out_error_offset);

// 按 key 查成员；不是对象、或没有这个 key 都返回 NULL。
const json_value_t *json_object_get(const json_value_t *object, const char *key);

// 带下标的数组访问，越界返回 NULL。
const json_value_t *json_array_at(const json_value_t *array, usize index);

// 取值。类型不符时返回 false 且**不写出参**，让调用方决定默认值。
bool json_as_bool(const json_value_t *v, bool *out);
bool json_as_f64(const json_value_t *v, f64 *out);
bool json_as_f32(const json_value_t *v, f32 *out);
bool json_as_i32(const json_value_t *v, i32 *out);
bool json_as_u32(const json_value_t *v, u32 *out);
bool json_as_string(const json_value_t *v, const char **out, usize *out_length);

// 常用组合：成员缺失或类型不符时取默认值，省去"先判 NULL 再判类型"的样板。
f32      json_get_f32(const json_value_t *object, const char *key, f32 fallback);
u32      json_get_u32(const json_value_t *object, const char *key, u32 fallback);
i32      json_get_i32(const json_value_t *object, const char *key, i32 fallback);
bool     json_get_bool(const json_value_t *object, const char *key, bool fallback);
const char *json_get_string(const json_value_t *object, const char *key, const char *fallback);

// ---------------------------------------------------------------- 生成
//
// 写入端挂在 bio_writer_t 上：缓冲区多大就写多少，越界时 writer 进错误态，
// 调用方在末尾统一判 json_writer_ok()。不会出现"写着写着踩出去"。
typedef struct json_writer_t {
    bio_writer_t *out;
    u32           indent_spaces; // 0 = 紧凑输出
    usize         depth;         // 当前打开的容器层数
    bool          failed;
    // 每层容器一个"还没有成员"标志：决定下一个成员前面要不要补逗号，
    // 以及收尾的 } / ] 前面要不要换行。用栈式的数组而不是"记住上一个
    // token"，是因为容器会嵌套，而深度有上限所以数组够用。
    bool empty[JSON_MAX_DEPTH + 1u];
    // 同形状的第二个数组：这一层是不是"成员挤在同一行"。
    // 只有"短数值数组"会用（向量的三个分量、八个槽位名），因为它们在一行里
    // 才是可读的；把 [0, -0.3, 0] 拆成四行会让场景文件的行数翻几倍,
    // 而那种文件的主要用途是人眼扫一遍 + 手改一个数。
    bool inline_at[JSON_MAX_DEPTH + 1u];
    bool pending_key; // 刚写完 "key": ，紧跟的值不该再写分隔符
} json_writer_t;

void json_writer_init(json_writer_t *w, bio_writer_t *out, u32 indent_spaces);
bool json_writer_ok(const json_writer_t *w);

bool json_write_null(json_writer_t *w);
bool json_write_bool(json_writer_t *w, bool value);
bool json_write_f32(json_writer_t *w, f32 value);
bool json_write_f64(json_writer_t *w, f64 value);
bool json_write_u32(json_writer_t *w, u32 value);
bool json_write_i32(json_writer_t *w, i32 value);
// 字符串按 UTF-8 写出，必要时转义。长度超界的输入会被拒绝而不是截断。
bool json_write_string(json_writer_t *w, const char *text);
bool json_write_string_n(json_writer_t *w, const char *text, usize length);

bool json_begin_object(json_writer_t *w);
bool json_end_object(json_writer_t *w);
bool json_begin_array(json_writer_t *w);
// 成员写在**同一行**的数组：`[0, -0.3, 0]`。
// 给"两三个短数值"那种容器用 —— 向量的三个分量各占一行会让场景文件涨成
// 原来的三倍长，而它的主要用途是人眼扫一遍再手改其中一个数。
// 只有数组有这一档：对象的键值对没有"三个字符一行"的阅读场景。
bool json_begin_array_inline(json_writer_t *w);
bool json_end_array(json_writer_t *w);
// 对象成员：先写 key，紧接着写那个值（值用上面任意一个 write_*）。
bool json_write_key(json_writer_t *w, const char *key);

#endif // ENGINE_CORE_JSON_H
