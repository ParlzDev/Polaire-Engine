// =============================================================================
//  tests/test_json.c —— 自己写的 JSON 解析 / 生成
//
//  重点是**负面用例**。正例只证明"我手写的那几个样例能读"，
//  而场景文件与 M7 的 GLTF 会读到各种人手工编辑过、被截断过、
//  被别的工具生成过的东西。解析器最怕的不是崩，是**部分成功**：
//  读出半份数据还报告成功，比直接拒绝破坏力大得多。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include <string.h>

#include "engine/core/json.h"

#define JSON_TEST_ARENA (256u * 1024u)

typedef struct json_fixture_t {
    allocator_tracking_t tracker;
    arena_t              arena;
} json_fixture_t;

static void fixture_begin(json_fixture_t *fx, const char *name)
{
    allocator_tracking_init(&fx->tracker, allocator_heap());
    EXPECT_TRUE(arena_init(&fx->arena, &fx->tracker.base, JSON_TEST_ARENA, name));
}

static void fixture_end(json_fixture_t *fx)
{
    arena_destroy(&fx->arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&fx->tracker));
}

static const json_value_t *parse_ok(json_fixture_t *fx, const char *text)
{
    const json_value_t *root = NULL;
    const char         *error = "?";
    usize               offset = 0u;
    const bool ok = json_parse_cstring(&fx->arena, text, &root, &error, &offset);
    if (!ok) {
        TF_CHECK(false, "解析应当成功但失败了：%s（偏移 %llu）\n  输入：%s", error,
                 (unsigned long long)offset, text);
        return NULL;
    }
    EXPECT_NOT_NULL(root);
    return root;
}

static void parse_fails(json_fixture_t *fx, const char *text, const char *what)
{
    const json_value_t *root = (const json_value_t *)0x1;
    const char         *error = NULL;
    usize               offset = 0u;
    const bool          ok = json_parse_cstring(&fx->arena, text, &root, &error, &offset);
    TF_CHECK(!ok, "坏输入被接受了（%s）：%s", what, text);
    TF_CHECK(root == NULL, "失败时却返回了根节点（%s）", what);
    EXPECT_NOT_NULL(error);
}

// TF_CHECK 失败后**不会**中止测试函数，所以解析失败还继续解引用就是段错误，
// 会把"第一条断言为什么失败"这个唯一有用的信息一起冲掉。
// 这个宏负责在拿到 NULL 时干净退出，让失败信息留在输出里而不是变成崩溃。
#define REQUIRE_ROOT(expr)                                                                         \
    const json_value_t *root = (expr);                                                             \
    if (root == NULL) {                                                                            \
        fixture_end(&fx);                                                                          \
        return;                                                                                    \
    }

// ---------------------------------------------------------------- 正例

static void json_parses_scalars_and_nesting(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_scalars");

    REQUIRE_ROOT(parse_ok(&fx, "{\"i\": 42, \"f\": -1.5, \"t\": true, "
                               "\"f2\": false, \"s\": \"hi\", \"n\": null}"));
    EXPECT_TRUE(root->kind == JSON_OBJECT);
    EXPECT_EQ_U64(root->item_count, 6u);

    u32   u = 0u;
    f32   f = 0.0f;
    bool  b = false;
    const char *s = NULL;
    EXPECT_TRUE(json_as_u32(json_object_get(root, "i"), &u));
    EXPECT_EQ_U64(u, 42u);
    EXPECT_TRUE(json_as_f32(json_object_get(root, "f"), &f));
    EXPECT_NEAR((double)f, -1.5, 1e-9);
    EXPECT_TRUE(json_as_bool(json_object_get(root, "t"), &b) && b);
    EXPECT_TRUE(json_as_bool(json_object_get(root, "f2"), &b) && !b);
    EXPECT_TRUE(json_as_string(json_object_get(root, "s"), &s, NULL));
    EXPECT_STR_EQ(s, "hi");
    EXPECT_TRUE(json_object_get(root, "n")->kind == JSON_NULL);

    // 带默认值的便捷读取：缺失与类型不符都走 fallback
    EXPECT_NEAR((double)json_get_f32(root, "missing", 3.25f), 3.25, 1e-9);
    EXPECT_NEAR((double)json_get_f32(root, "s", 7.0f), 7.0, 1e-9); // 类型不符
    EXPECT_EQ_U64(json_get_u32(root, "nope", 9u), 9u);

    fixture_end(&fx);
}

static void json_parses_arrays_and_empty_containers(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_arrays");

    REQUIRE_ROOT(parse_ok(&fx, "[1, 2, [3, [4]], {}, [], \"x\"]"));
    EXPECT_TRUE(root->kind == JSON_ARRAY);
    EXPECT_EQ_U64(root->item_count, 6u);

    u32 v = 0u;
    EXPECT_TRUE(json_as_u32(json_array_at(root, 0u), &v) && v == 1u);
    const json_value_t *nested = json_array_at(root, 2u);
    EXPECT_TRUE(nested->kind == JSON_ARRAY);
    EXPECT_EQ_U64(nested->item_count, 2u);
    EXPECT_TRUE(json_array_at(nested, 1u)->kind == JSON_ARRAY);
    EXPECT_TRUE(json_array_at(root, 3u)->item_count == 0u); // {}
    EXPECT_TRUE(json_array_at(root, 4u)->item_count == 0u); // []
    EXPECT_NULL(json_array_at(root, 99u));                  // 越界返回 NULL 而不是崩

    fixture_end(&fx);
}

static void json_decodes_escapes(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_escapes");

    REQUIRE_ROOT(parse_ok(&fx,
                          "{\"a\": \"x\\ny\", \"b\": \"q\\\"r\", \"c\": \"tab\\there\", "
                          "\"d\": \"\\\\\", \"e\": \"\\u0041\"}"));
    const char *s = NULL;
    usize       len = 0u;
    EXPECT_TRUE(json_as_string(json_object_get(root, "a"), &s, &len));
    EXPECT_TRUE(len == 3u && s[1] == '\n');
    EXPECT_TRUE(json_as_string(json_object_get(root, "b"), &s, &len));
    EXPECT_TRUE(len == 3u && s[1] == '"');
    EXPECT_TRUE(json_as_string(json_object_get(root, "c"), &s, &len));
    EXPECT_EQ_U64(len, 8u); // "tab" + TAB + "here" = 3 + 1 + 4
    EXPECT_TRUE(len == 8u && s[3] == '\t');
    EXPECT_TRUE(json_as_string(json_object_get(root, "d"), &s, &len));
    EXPECT_TRUE(len == 1u && s[0] == '\\');
    EXPECT_TRUE(json_as_string(json_object_get(root, "e"), &s, &len));
    EXPECT_STR_EQ(s, "A");

    // 非 ASCII：BMP 内 3 字节、补充平面走代理对（4 字节 UTF-8）
    root = parse_ok(&fx, "[\"\\u4e2d\", \"\\uD83D\\uDE00\"]");
    EXPECT_TRUE(json_as_string(json_array_at(root, 0u), &s, &len));
    EXPECT_EQ_U64(len, 3u); // "中" 的 UTF-8 是 3 字节
    EXPECT_TRUE(json_as_string(json_array_at(root, 1u), &s, &len));
    EXPECT_EQ_U64(len, 4u); // 代理对合成一个 4 字节码点
    EXPECT_EQ_U64((u8)s[0], 0xF0u);

    fixture_end(&fx);
}

// ---------------------------------------------------------------- 负面用例

static void json_rejects_malformed_numbers(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_numbers");

    parse_fails(&fx, "[01]", "前导零");
    parse_fails(&fx, "[1.]", "小数点后无数字");
    parse_fails(&fx, "[.5]", "缺少整数部分");
    parse_fails(&fx, "[1e]", "指数缺数字");
    parse_fails(&fx, "[-]", "只有负号");
    parse_fails(&fx, "[1e999]", "溢出成 inf");
    parse_fails(&fx, "[NaN]", "NaN 不是 JSON");
    parse_fails(&fx, "[Infinity]", "Infinity 不是 JSON");
    parse_fails(&fx, "[1 2]", "两个数字没有分隔符");

    // 合法的边界：0、-0、1e3、0.0
    EXPECT_NOT_NULL(parse_ok(&fx, "[0, -0, 1e3, 0.0, -1.5e-3]"));

    fixture_end(&fx);
}

static void json_rejects_truncation_and_trailing_garbage(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_truncation");

    parse_fails(&fx, "{\"a\": ", "值缺失");
    parse_fails(&fx, "[1, 2", "数组没收尾");
    parse_fails(&fx, "{\"a\": 1", "对象没收尾");
    parse_fails(&fx, "\"abc", "字符串没收尾");
    parse_fails(&fx, "", "空输入");
    parse_fails(&fx, "{} {}", "根值之后还有内容");
    parse_fails(&fx, "{\"a\": 1} 乱码", "尾部垃圾");
    parse_fails(&fx, "{\"a\": 1,}", "尾逗号");
    parse_fails(&fx, "[,]", "首逗号");

    fixture_end(&fx);
}

static void json_rejects_bad_strings(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_bad_strings");

    parse_fails(&fx, "[\"a\\qb\"]", "未知转义");
    parse_fails(&fx, "[\"a\\u12\"]", "\\u 不足 4 位");
    parse_fails(&fx, "[\"a\\uZZZZ\"]", "\\u 不是十六进制");
    parse_fails(&fx, "[\"a\\uD83D\"]", "落单的高位代理");
    parse_fails(&fx, "[\"a\\uDE00\"]", "落单的低位代理");
    // 高位代理后面必须紧跟低位代理。\uFFFF 本身是合法码点（所以
    // "a\uFFFFF" 其实是合法 JSON，不能拿它当负面用例），
    // 这里用"高位代理 + 非低位代理"覆盖真正非法的那条分支。
    parse_fails(&fx, "[\"a\\uD83D\\u0041\"]", "高位代理后不是低位代理");

    // 裸控制字符（未转义的换行）必须拒绝：它几乎总是上游忘了转义，
    // 放过去会让坏文件"看起来解析成功"。
    parse_fails(&fx, "[\"a\nb\"]", "未转义的控制字符");

    fixture_end(&fx);
}

static void json_rejects_duplicate_keys(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_dup_keys");

    // 静默取后一个会掩盖编辑器的 bug：面板写了两次同一个字段，
    // 存档里却只剩一份数据，谁都不知道丢了。
    parse_fails(&fx, "{\"a\":1,\"a\":2}", "重复 key");
    parse_fails(&fx, "{\"a\":1, \"b\":2, \"a\":3}", "不相邻的重复 key");
    EXPECT_NOT_NULL(parse_ok(&fx, "{\"a\":1,\"b\":2,\"c\":3}"));

    fixture_end(&fx);
}

static void json_rejects_excessive_depth(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_depth");

    // 递归下降解析器没有深度上限就会在 10 万个 '[' 上把栈打穿。
    // 这里构造一个"合法但超深"的输入，必须被拒而不是崩。
    char deep[JSON_MAX_DEPTH * 2u + 16u];
    usize n = 0u;
    for (usize i = 0; i < JSON_MAX_DEPTH + 8u; ++i) {
        deep[n++] = '[';
    }
    deep[n] = '\0';
    parse_fails(&fx, deep, "嵌套过深");

    // 刚好在上限内的必须能读（否则上限就定得太小）
    char ok_buf[JSON_MAX_DEPTH * 2u + 16u];
    n = 0u;
    for (usize i = 0; i < JSON_MAX_DEPTH - 1u; ++i) {
        ok_buf[n++] = '[';
    }
    for (usize i = 0; i < JSON_MAX_DEPTH - 1u; ++i) {
        ok_buf[n++] = ']';
    }
    ok_buf[n] = '\0';
    EXPECT_NOT_NULL(parse_ok(&fx, ok_buf));

    fixture_end(&fx);
}

// ---------------------------------------------------------------- 生成

// 把值写进一个**刚好够大**的缓冲区，再解析回来逐字段比对。
static void json_writer_roundtrip(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_write");

    u8 buffer[512];
    bio_writer_t bio;
    bio_writer_init(&bio, buffer, sizeof(buffer));

    json_writer_t w;
    json_writer_init(&w, &bio, 0u); // 紧凑输出

    bool ok = json_begin_object(&w) && json_write_key(&w, "count") && json_write_u32(&w, 7u) &&
              json_write_key(&w, "ratio") && json_write_f32(&w, 0.1f) &&
              json_write_key(&w, "name") && json_write_string(&w, "a\"b\\c\nd") &&
              json_write_key(&w, "flag") && json_write_bool(&w, true) &&
              json_write_key(&w, "list") && json_begin_array(&w) &&
              json_write_i32(&w, -1) && json_write_i32(&w, 2) && json_end_array(&w) &&
              json_write_key(&w, "nested") && json_begin_object(&w) &&
              json_write_key(&w, "empty") && json_begin_array(&w) && json_end_array(&w) &&
              json_end_object(&w) && json_end_object(&w);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(json_writer_ok(&w));
    EXPECT_LT_U64(bio_writer_used(&bio), sizeof(buffer));

    buffer[bio_writer_used(&bio)] = '\0';
    const char *text = (const char *)buffer;
    EXPECT_STR_EQ(text, "{\"count\":7,\"ratio\":0.1,\"name\":\"a\\\"b\\\\c\\nd\","
                        "\"flag\":true,\"list\":[-1,2],\"nested\":{\"empty\":[]}}");

    REQUIRE_ROOT(parse_ok(&fx, text));
    EXPECT_EQ_U64(json_get_u32(root, "count", 0u), 7u);
    // 和 (double)0.1f 比而不是和 0.1 比：0.1f 的精确值是
    // 0.100000001490116...，拿 double 字面量 0.1 配 1e-9 容差本身就必输。
    // "能不能精确往返"由 floats_round_trip_exactly 逐位验证。
    EXPECT_NEAR((double)json_get_f32(root, "ratio", 0.0f), (double)0.1f, 1e-9);
    EXPECT_STR_EQ(json_get_string(root, "name", "?"), "a\"b\\c\nd");
    EXPECT_TRUE(json_get_bool(root, "flag", false));
    const json_value_t *list = json_object_get(root, "list");
    EXPECT_EQ_U64(list->item_count, 2u);
    EXPECT_EQ_U64(json_array_at(list, 0u)->kind, JSON_NUMBER);
    EXPECT_EQ_U64(json_object_get(root, "nested")->item_count, 1u);

    fixture_end(&fx);
}

// f32 用 %.9g：必须能原样读回，这是"存了读、读了再存逐字节一致"的前提。
static void json_floats_round_trip_exactly(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_float_exact");

    static const f32 samples[] = {0.0f,     1.0f,      -1.0f,   0.1f,      1.0f / 3.0f,
                                  1e-8f,    1e8f,      3.40282347e+38f, -1.23456789f,
                                  16777217.0f}; // 2^24+1：整数精度边界上最能暴露问题
    const usize count = sizeof(samples) / sizeof(samples[0]);

    for (usize i = 0; i < count; ++i) {
        u8 buffer[64];
        bio_writer_t bio;
        bio_writer_init(&bio, buffer, sizeof(buffer));
        json_writer_t w;
        json_writer_init(&w, &bio, 0u);
        EXPECT_TRUE(json_begin_array(&w) && json_write_f32(&w, samples[i]) && json_end_array(&w));
        EXPECT_TRUE(json_writer_ok(&w));
        buffer[bio_writer_used(&bio)] = '\0';

        const json_value_t *root = parse_ok(&fx, (const char *)buffer);
        f32 back = 0.0f;
        EXPECT_TRUE(json_as_f32(json_array_at(root, 0u), &back));
        // 逐位相等，不是"接近"：文本协议丢精度的话，存档每读一次就漂一点
        EXPECT_TRUE(memcmp(&back, &samples[i], sizeof(f32)) == 0);
    }

    fixture_end(&fx);
}

// NaN / Infinity 不是 JSON：写它们会产出不合法的文档，所以必须拒绝。
static void json_writer_rejects_non_finite(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_write_nan");

    const f32 nan = (f32)(0.0 / 0.0);
    const f32 inf = (f32)(1.0 / 0.0);

    u8 buffer[64];
    bio_writer_t bio;
    bio_writer_init(&bio, buffer, sizeof(buffer));
    json_writer_t w;
    json_writer_init(&w, &bio, 0u);
    EXPECT_FALSE(json_write_f32(&w, nan));
    EXPECT_FALSE(json_writer_ok(&w)); // 已经进错误态

    bio_writer_init(&bio, buffer, sizeof(buffer));
    json_writer_init(&w, &bio, 0u);
    EXPECT_FALSE(json_write_f32(&w, inf));
    EXPECT_FALSE(json_writer_ok(&w));

    fixture_end(&fx);
}

// 缓冲区太小必须**报失败**，而不是写出去踩到别人身上。
// 这一条是 M4 那个教训的正面版本：往调用方给的缓冲区里写，就得知道它多大。
static void json_writer_does_not_overflow_buffer(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_write_bounded");

    u8 buffer[16];
    memset(buffer, 0xAB, sizeof(buffer));

    bio_writer_t bio;
    bio_writer_init(&bio, buffer, sizeof(buffer));
    json_writer_t w;
    json_writer_init(&w, &bio, 0u);

    EXPECT_FALSE(json_write_string(&w, "0123456789012345678901234567890123456789"));
    EXPECT_FALSE(json_writer_ok(&w));
    // 核心不变式：写入量永远不超过缓冲区大小，且第一个字节确实写进去了
    EXPECT_LE_U64(bio_writer_used(&bio), sizeof(buffer));
    EXPECT_EQ_U64(buffer[0], (usize)'"');

    fixture_end(&fx);
}

// 美化输出的缩进与逗号位置：编辑器与"人肉看存档"依赖它
static void json_pretty_output_layout(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_pretty");

    u8 buffer[256];
    bio_writer_t bio;
    bio_writer_init(&bio, buffer, sizeof(buffer));
    json_writer_t w;
    json_writer_init(&w, &bio, 2u);

    EXPECT_TRUE(json_begin_object(&w) && json_write_key(&w, "a") && json_write_u32(&w, 1u) &&
                json_write_key(&w, "b") && json_begin_array(&w) && json_write_u32(&w, 2u) &&
                json_write_u32(&w, 3u) && json_end_array(&w) &&
                json_write_key(&w, "c") && json_begin_object(&w) && json_end_object(&w) &&
                json_end_object(&w));
    EXPECT_TRUE(json_writer_ok(&w));
    buffer[bio_writer_used(&bio)] = '\0';

    EXPECT_STR_EQ((const char *)buffer,
                  "{\n  \"a\": 1,\n  \"b\": [\n    2,\n    3\n  ],\n  \"c\": {}\n}");

    // 美化输出同样能读回来（缩进不影响语义）
    EXPECT_NOT_NULL(parse_ok(&fx, (const char *)buffer));

    fixture_end(&fx);
}

// 控制字符必须转义，否则产出的不是合法 JSON
static void json_writer_escapes_control_chars(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_write_ctrl");

    u8 buffer[128];
    bio_writer_t bio;
    bio_writer_init(&bio, buffer, sizeof(buffer));
    json_writer_t w;
    json_writer_init(&w, &bio, 0u);

    char text[4] = {'a', (char)0x01, 'b', '\0'};
    EXPECT_TRUE(json_write_string(&w, text));
    EXPECT_TRUE(json_writer_ok(&w));
    buffer[bio_writer_used(&bio)] = '\0';

    EXPECT_STR_EQ((const char *)buffer, "\"a\\u0001b\"");
    const json_value_t *root = parse_ok(&fx, (const char *)buffer);
    const char        *s = NULL;
    usize              len = 0u;
    EXPECT_TRUE(json_as_string(root, &s, &len));
    EXPECT_EQ_U64(len, 3u);
    EXPECT_EQ_U64((u8)s[1], 1u);

    fixture_end(&fx);
}

// 同一行的数组：向量的三个分量各占一行会让场景文件涨成三倍长，
// 而那种文件的主要用途就是"扫一眼，改掉其中一个数"。
static void json_writer_inline_arrays(void)
{
    json_fixture_t fx;
    fixture_begin(&fx, "json_inline");

    u8 buffer[256];
    bio_writer_t bio;
    bio_writer_init(&bio, buffer, sizeof(buffer));
    json_writer_t w;
    json_writer_init(&w, &bio, 2u);

    EXPECT_TRUE(json_begin_object(&w) && json_write_key(&w, "pos") &&
                json_begin_array_inline(&w) && json_write_u32(&w, 0u) &&
                json_write_f32(&w, -0.5f) && json_write_u32(&w, 0u) && json_end_array(&w) &&
                json_write_key(&w, "rows") && json_begin_array(&w) && json_write_u32(&w, 1u) &&
                json_write_u32(&w, 2u) && json_end_array(&w) &&
                json_write_key(&w, "none") && json_begin_array_inline(&w) && json_end_array(&w) &&
                json_end_object(&w));
    EXPECT_TRUE(json_writer_ok(&w));
    buffer[bio_writer_used(&bio)] = '\0';

    EXPECT_STR_EQ((const char *)buffer,
                  "{\n  \"pos\": [0, -0.5, 0],\n  \"rows\": [\n    1,\n    2\n  ],\n"
                  "  \"none\": []\n}");

    // 排版换了，语义没换：读回来仍然是三个分量，而且普通数组照旧分行
    const json_value_t *root = parse_ok(&fx, (const char *)buffer);
    EXPECT_NOT_NULL(root);
    if (root != NULL) {
        const json_value_t *pos = json_object_get(root, "pos");
        EXPECT_EQ_U64(pos->kind == JSON_ARRAY, 1u);
        EXPECT_EQ_U64(pos->item_count, 3u);
        EXPECT_NEAR((double)json_get_f32(pos, "x", 99.0f), 99.0, 1e-6); // 数组没有 key，取默认
        f32 second = 0.0f;
        EXPECT_TRUE(json_as_f32(json_array_at(pos, 1u), &second));
        EXPECT_NEAR((double)second, -0.5, 1e-6);
        EXPECT_EQ_U64(json_object_get(root, "none")->item_count, 0u);
    }

    fixture_end(&fx);
}

// =============================================================================
void test_json_register(void)
{
    test_register("json", "parses_scalars_and_nesting", json_parses_scalars_and_nesting);
    test_register("json", "parses_arrays_and_empty_containers",
                  json_parses_arrays_and_empty_containers);
    test_register("json", "decodes_escapes", json_decodes_escapes);
    test_register("json", "rejects_malformed_numbers", json_rejects_malformed_numbers);
    test_register("json", "rejects_truncation_and_trailing_garbage",
                  json_rejects_truncation_and_trailing_garbage);
    test_register("json", "rejects_bad_strings", json_rejects_bad_strings);
    test_register("json", "rejects_duplicate_keys", json_rejects_duplicate_keys);
    test_register("json", "rejects_excessive_depth", json_rejects_excessive_depth);
    test_register("json", "writer_roundtrip", json_writer_roundtrip);
    test_register("json", "floats_round_trip_exactly", json_floats_round_trip_exactly);
    test_register("json", "writer_rejects_non_finite", json_writer_rejects_non_finite);
    test_register("json", "writer_does_not_overflow_buffer", json_writer_does_not_overflow_buffer);
    test_register("json", "pretty_output_layout", json_pretty_output_layout);
    test_register("json", "writer_escapes_control_chars", json_writer_escapes_control_chars);
    test_register("json", "writer_inline_arrays_stay_on_one_line", json_writer_inline_arrays);
}
