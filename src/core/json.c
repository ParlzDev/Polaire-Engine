// =============================================================================
//  src/core/json.c —— 自己写的 JSON 解析 / 生成
//
//  解析是递归下降。深度有上限（JSON_MAX_DEPTH），所以递归不会把栈打穿 ——
//  这条和 M5 世界变换"不用递归"是同一个考虑：栈溢出崩的位置离真正的错误
//  十万八千里，M4 刚为这类问题查了一整晚。
// =============================================================================
#include "engine/core/json.h"

#include <math.h>  // isfinite / FLT_MAX 相关的溢出判断在 json_as_f32 里
#include <stdio.h>
#include <stdlib.h> // strtod / strtof
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"

#define JSON_SCRATCH_CAPACITY 48u // 数字 token 的暂存；再长就不是合法/有用的数字了

// 容器元素数上界。坏输入会让解析器按"剩余字节/2"估容量，
// 上限压到 100 万，超了直接拒 —— arena 分配失败虽然不会踩内存，
// 但一个 2KB 的文件申请 128MB 本身就说明它是坏的。
#define JSON_MAX_CONTAINER_ITEMS 1000000u

const char *json_kind_name(json_kind_t kind)
{
    switch (kind) {
    case JSON_NULL: return "null";
    case JSON_BOOL: return "bool";
    case JSON_NUMBER: return "number";
    case JSON_STRING: return "string";
    case JSON_ARRAY: return "array";
    case JSON_OBJECT: return "object";
    case JSON_KIND_COUNT: break;
    }
    return "?";
}

// =============================================================================
//  解析
// =============================================================================
typedef struct json_parser_t {
    arena_t       *arena;
    const char    *text;
    usize          length;
    usize          pos;
    u32            depth;
    const char    *error;
    usize          error_at;
} json_parser_t;

static void json_fail(json_parser_t *p, const char *why)
{
    if (p->error == NULL) { // 保留第一个错误：后面的都是它的连锁反应
        p->error = why;
        p->error_at = p->pos;
    }
}

static bool json_at_end(const json_parser_t *p)
{
    return p->pos >= p->length;
}

static void json_skip_ws(json_parser_t *p)
{
    while (p->pos < p->length) {
        const char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static json_value_t *json_new_value(json_parser_t *p)
{
    json_value_t *v = ARENA_NEW(p->arena, json_value_t);
    if (v != NULL) {
        v->kind = JSON_NULL;
    } else {
        json_fail(p, "arena 已耗尽");
    }
    return v;
}

static bool json_expect_literal(json_parser_t *p, const char *literal)
{
    const usize n = strlen(literal);
    if (p->length - p->pos < n || memcmp(p->text + p->pos, literal, n) != 0) {
        return false;
    }
    p->pos += n;
    return true;
}

// ---------------------------------------------------------------- 字符串

static bool json_hex4(json_parser_t *p, u32 *out)
{
    if (p->length - p->pos < 4u) {
        return false;
    }
    u32 value = 0u;
    for (u32 i = 0; i < 4u; ++i) {
        const char c = p->text[p->pos + i];
        u32          digit = 0u;
        if (c >= '0' && c <= '9') {
            digit = (u32)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (u32)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (u32)(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    p->pos += 4u;
    *out = value;
    return true;
}

// UTF-8 编码一个码点。返回写入的字节数，0 表示码点非法。
static usize json_encode_utf8(u32 codepoint, u8 *out)
{
    if (codepoint <= 0x7Fu) {
        out[0] = (u8)codepoint;
        return 1u;
    }
    if (codepoint <= 0x7FFu) {
        out[0] = (u8)(0xC0u | (codepoint >> 6));
        out[1] = (u8)(0x80u | (codepoint & 0x3Fu));
        return 2u;
    }
    // 代理区（D800..DFFF）不能单独出现：落单的 surrogate 视为非法。
    if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
        return 0u;
    }
    if (codepoint <= 0xFFFFu) {
        out[0] = (u8)(0xE0u | (codepoint >> 12));
        out[1] = (u8)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = (u8)(0x80u | (codepoint & 0x3Fu));
        return 3u;
    }
    if (codepoint <= 0x10FFFFu) {
        out[0] = (u8)(0xF0u | (codepoint >> 18));
        out[1] = (u8)(0x80u | ((codepoint >> 12) & 0x3Fu));
        out[2] = (u8)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[3] = (u8)(0x80u | (codepoint & 0x3Fu));
        return 4u;
    }
    return 0u;
}

// 解码一个字符串字面量（p->pos 指向开头的引号）。
// 结果拷贝进 arena，长度含结尾 NUL。
static bool json_parse_string(json_parser_t *p, const char **out_string, usize *out_length)
{
    if (out_string != NULL) {
        *out_string = NULL;
    }
    if (out_length != NULL) {
        *out_length = 0u;
    }
    if (json_at_end(p) || p->text[p->pos] != '"') {
        json_fail(p, "期望字符串");
        return false;
    }
    p->pos++;

    // 转义只会让结果变短，所以源切片长度就是输出的上界
    //（\uXXXX 6 个字节最多产出 4 字节 UTF-8）。
    const usize  start = p->pos;
    const usize  slice = p->length - start;
    char        *buffer = (char *)arena_alloc(p->arena, slice + 1u);
    if (buffer == NULL) {
        json_fail(p, "arena 已耗尽");
        return false;
    }

    usize out = 0u;
    bool  closed = false;
    while (p->pos < p->length) {
        const char c = p->text[p->pos];

        if (c == '"') {
            p->pos++;
            closed = true;
            break;
        }

        // 裸控制字符必须报错：这几乎总是因为上游忘了转义，
        // 放过去会让一个坏文件"看起来解析成功"。
        if ((u8)c < 0x20u) {
            json_fail(p, "字符串里有未转义的控制字符");
            return false;
        }

        if (c != '\\') {
            buffer[out++] = c;
            p->pos++;
            continue;
        }

        p->pos++;
        if (p->pos >= p->length) {
            json_fail(p, "转义序列被截断");
            return false;
        }
        const char e = p->text[p->pos++];
        switch (e) {
        case '"': buffer[out++] = '"'; break;
        case '\\': buffer[out++] = '\\'; break;
        case '/': buffer[out++] = '/'; break;
        case 'b': buffer[out++] = '\b'; break;
        case 'f': buffer[out++] = '\f'; break;
        case 'n': buffer[out++] = '\n'; break;
        case 'r': buffer[out++] = '\r'; break;
        case 't': buffer[out++] = '\t'; break;
        case 'u': {
            u32  codepoint = 0u;
            u8   encoded[4];
            if (!json_hex4(p, &codepoint)) {
                json_fail(p, "\\u 后面不是 4 位十六进制");
                return false;
            }
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                // 高位代理：必须紧跟一个低位代理，否则码点不完整
                if (p->length - p->pos < 6u || p->text[p->pos] != '\\' ||
                    p->text[p->pos + 1] != 'u') {
                    json_fail(p, "落单的高位代理");
                    return false;
                }
                p->pos += 2u;
                u32 low = 0u;
                if (!json_hex4(p, &low) || low < 0xDC00u || low > 0xDFFFu) {
                    json_fail(p, "高位代理后面不是低位代理");
                    return false;
                }
                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
            }
            const usize wrote = json_encode_utf8(codepoint, encoded);
            if (wrote == 0u) {
                json_fail(p, "\\u 码点非法");
                return false;
            }
            for (usize i = 0; i < wrote; ++i) {
                buffer[out++] = (char)encoded[i];
            }
            break;
        }
        default: json_fail(p, "未知的转义字符"); return false;
        }
    }

    if (!closed) {
        json_fail(p, "字符串没有收尾引号");
        return false;
    }

    buffer[out] = '\0';
    if (out_string != NULL) {
        *out_string = buffer;
    }
    if (out_length != NULL) {
        *out_length = out;
    }
    return true;
}

// ---------------------------------------------------------------- 数字
//
// 扫描 token 后交给 strtod：手写浮点转换很容易在舍入上差一个 ULP，
// 而这里的硬性要求是"写出去能原样读回来"。
static bool json_parse_number(json_parser_t *p, f64 *out)
{
    char  scratch[JSON_SCRATCH_CAPACITY];
    usize n = 0u;

    if (!json_at_end(p) && p->text[p->pos] == '-') {
        scratch[n++] = '-';
        p->pos++;
    }

    // 整数部分。
    usize digits_begin = n;
    while (!json_at_end(p) && p->text[p->pos] >= '0' && p->text[p->pos] <= '9') {
        if (n + 1u >= sizeof(scratch)) {
            json_fail(p, "数字过长");
            return false;
        }
        scratch[n++] = p->text[p->pos++];
    }
    if (n == digits_begin) {
        json_fail(p, "数字缺少整数部分");
        return false;
    }
    // 前导零非法：0.5 可以，0123 不行。放过去会让同一个字节串
    // 在不同解析器里读出不同的值。
    if (n - digits_begin > 1u && scratch[digits_begin] == '0') {
        json_fail(p, "整数部分不允许前导零");
        return false;
    }

    if (!json_at_end(p) && p->text[p->pos] == '.') {
        scratch[n++] = '.';
        p->pos++;
        usize frac_begin = n;
        while (!json_at_end(p) && p->text[p->pos] >= '0' && p->text[p->pos] <= '9') {
            if (n + 1u >= sizeof(scratch)) {
                json_fail(p, "数字过长");
                return false;
            }
            scratch[n++] = p->text[p->pos++];
        }
        if (n == frac_begin) {
            json_fail(p, "小数点后没有数字");
            return false;
        }
    }

    if (!json_at_end(p) && (p->text[p->pos] == 'e' || p->text[p->pos] == 'E')) {
        scratch[n++] = 'e';
        p->pos++;
        if (!json_at_end(p) && (p->text[p->pos] == '+' || p->text[p->pos] == '-')) {
            scratch[n++] = p->text[p->pos++];
        }
        usize exp_begin = n;
        while (!json_at_end(p) && p->text[p->pos] >= '0' && p->text[p->pos] <= '9') {
            if (n + 1u >= sizeof(scratch)) {
                json_fail(p, "数字过长");
                return false;
            }
            scratch[n++] = p->text[p->pos++];
        }
        if (n == exp_begin) {
            json_fail(p, "指数部分缺少数字");
            return false;
        }
    }

    scratch[n] = '\0';

    char *end = NULL;
    const f64 value = strtod(scratch, &end);
    if (end != scratch + n) {
        json_fail(p, "数字格式非法");
        return false;
    }
    // JSON 没有 Infinity / NaN 这两个字面量。1e400 这类溢出成 inf 的
    // 文本必须拒绝，否则它会一路静默流到光照计算里。
    if (!isfinite(value)) {
        json_fail(p, "数字超出可表示范围");
        return false;
    }

    *out = value;
    return true;
}

static json_value_t *json_parse_value(json_parser_t *p);

static json_value_t *json_parse_array(json_parser_t *p)
{
    // p->pos 指向 '['
    p->pos++;

    json_value_t *value = json_new_value(p);
    if (value == NULL) {
        return NULL;
    }
    value->kind = JSON_ARRAY;

    // 元素个数事先不知道，所以按"剩余字节数 / 2"取上界（每个元素至少
    // 1 字节 + 一个分隔符），一次性从 arena 开出指针数组。
    // 上界只用于**分配**，填充仍以实际遇到的元素个数为准；
    // 真正的守卫是 JSON_MAX_CONTAINER_ITEMS —— 坏文件不该让解析器申请巨量内存。
    const usize capacity_bound = (p->length - p->pos) / 2u + 1u;
    if (capacity_bound > JSON_MAX_CONTAINER_ITEMS) {
        json_fail(p, "数组过大");
        return NULL;
    }

    json_value_t **items =
        (json_value_t **)arena_alloc(p->arena, sizeof(json_value_t *) * capacity_bound);
    if (items == NULL) {
        json_fail(p, "arena 已耗尽");
        return NULL;
    }

    usize count = 0u;
    json_skip_ws(p);
    if (!json_at_end(p) && p->text[p->pos] == ']') {
        p->pos++;
        value->items = items;
        value->item_count = 0u;
        return value;
    }

    for (;;) {
        if (count >= capacity_bound) {
            json_fail(p, "数组元素数超出界限");
            return NULL;
        }
        json_value_t *item = json_parse_value(p);
        if (item == NULL) {
            return NULL;
        }
        items[count++] = item;

        json_skip_ws(p);
        if (json_at_end(p)) {
            json_fail(p, "数组没有收尾 ]");
            return NULL;
        }
        const char c = p->text[p->pos++];
        if (c == ',') {
            json_skip_ws(p);
            continue;
        }
        if (c == ']') {
            break;
        }
        json_fail(p, "数组里期望 , 或 ]");
        return NULL;
    }

    value->items = items;
    value->item_count = count;
    return value;
}

static json_value_t *json_parse_object(json_parser_t *p)
{
    p->pos++; // '{'

    json_value_t *value = json_new_value(p);
    if (value == NULL) {
        return NULL;
    }
    value->kind = JSON_OBJECT;

    const usize capacity_bound = (p->length - p->pos) / 4u + 1u;
    if (capacity_bound > JSON_MAX_CONTAINER_ITEMS) {
        json_fail(p, "对象过大");
        return NULL;
    }

    json_value_t **items =
        (json_value_t **)arena_alloc(p->arena, sizeof(json_value_t *) * capacity_bound);
    const char  **keys =
        (const char **)arena_alloc(p->arena, sizeof(const char *) * capacity_bound);
    if (items == NULL || keys == NULL) {
        json_fail(p, "arena 已耗尽");
        return NULL;
    }

    usize count = 0u;
    json_skip_ws(p);
    if (!json_at_end(p) && p->text[p->pos] == '}') {
        p->pos++;
        value->items = items;
        value->keys = keys;
        value->item_count = 0u;
        return value;
    }

    for (;;) {
        json_skip_ws(p);
        if (count >= capacity_bound) {
            json_fail(p, "对象成员数超出界限");
            return NULL;
        }

        const char *key = NULL;
        usize       key_length = 0u;
        if (!json_parse_string(p, &key, &key_length)) {
            return NULL; // json_parse_string 已经写好了错误原因
        }

        // 重复 key 一律拒绝。静默取后一个会掩盖编辑器的 bug：
        // 面板写了两次同一个字段，存档里却只有一份数据，谁都不知道。
        // 成员数在场景文件里是个位数到几十，线性比较的代价可以忽略。
        for (usize i = 0; i < count; ++i) {
            if (keys[i] != NULL && strcmp(keys[i], key) == 0) {
                json_fail(p, "对象里有重复的 key");
                return NULL;
            }
        }

        json_skip_ws(p);
        if (json_at_end(p) || p->text[p->pos] != ':') {
            json_fail(p, "key 后面期望 :");
            return NULL;
        }
        p->pos++;

        json_value_t *member = json_parse_value(p);
        if (member == NULL) {
            return NULL;
        }

        keys[count] = key;
        items[count] = member;
        count++;

        json_skip_ws(p);
        if (json_at_end(p)) {
            json_fail(p, "对象没有收尾 }");
            return NULL;
        }
        const char c = p->text[p->pos++];
        if (c == ',') {
            continue;
        }
        if (c == '}') {
            break;
        }
        json_fail(p, "对象里期望 , 或 }");
        return NULL;
    }

    value->items = items;
    value->keys = keys;
    value->item_count = count;
    return value;
}

static json_value_t *json_parse_value(json_parser_t *p)
{
    json_skip_ws(p);
    if (p->error != NULL || json_at_end(p)) {
        if (p->error == NULL) {
            json_fail(p, "内容意外结束");
        }
        return NULL;
    }

    if (p->depth >= JSON_MAX_DEPTH) {
        json_fail(p, "嵌套深度超限");
        return NULL;
    }

    const char c = p->text[p->pos];

    if (c == '{' || c == '[') {
        p->depth++;
        json_value_t *container = (c == '{') ? json_parse_object(p) : json_parse_array(p);
        p->depth--;
        return container;
    }
    if (c == '"') {
        json_value_t *value = json_new_value(p);
        if (value == NULL) {
            return NULL;
        }
        value->kind = JSON_STRING;
        if (!json_parse_string(p, &value->string, &value->string_length)) {
            return NULL;
        }
        return value;
    }
    if (c == 't') {
        if (!json_expect_literal(p, "true")) {
            json_fail(p, "字面量拼写错误");
            return NULL;
        }
        json_value_t *value = json_new_value(p);
        if (value != NULL) {
            value->kind = JSON_BOOL;
            value->boolean = true;
        }
        return value;
    }
    if (c == 'f') {
        if (!json_expect_literal(p, "false")) {
            json_fail(p, "字面量拼写错误");
            return NULL;
        }
        json_value_t *value = json_new_value(p);
        if (value != NULL) {
            value->kind = JSON_BOOL;
            value->boolean = false;
        }
        return value;
    }
    if (c == 'n') {
        if (!json_expect_literal(p, "null")) {
            json_fail(p, "字面量拼写错误");
            return NULL;
        }
        return json_new_value(p); // kind 已是 JSON_NULL
    }

    // 剩下只可能是数字
    json_value_t *value = json_new_value(p);
    if (value == NULL) {
        return NULL;
    }
    value->kind = JSON_NUMBER;
    if (!json_parse_number(p, &value->number)) {
        return NULL;
    }
    return value;
}

bool json_parse(arena_t *arena, const char *text, usize length, const json_value_t **out_root,
                const char **out_error, usize *out_error_offset)
{
    if (out_root != NULL) {
        *out_root = NULL;
    }
    if (arena == NULL || text == NULL) {
        if (out_error != NULL) {
            *out_error = "参数为空";
        }
        return false;
    }

    json_parser_t p;
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.text = text;
    p.length = length;

    json_value_t *root = json_parse_value(&p);
    if (root == NULL) {
        if (out_error != NULL) {
            *out_error = (p.error != NULL) ? p.error : "解析失败";
        }
        if (out_error_offset != NULL) {
            *out_error_offset = p.error_at;
        }
        return false;
    }

    // 尾部垃圾必须拒绝：`{} {}` 或 `{"a":1} 乱码` 都不是合法文档。
    // 不查的话，一个被追加了内容的文件会"解析成功"而内容丢掉一半。
    json_skip_ws(&p);
    if (!json_at_end(&p)) {
        if (out_error != NULL) {
            *out_error = "根值之后还有内容";
        }
        if (out_error_offset != NULL) {
            *out_error_offset = p.pos;
        }
        return false;
    }

    if (out_root != NULL) {
        *out_root = root;
    }
    if (out_error != NULL) {
        *out_error = NULL;
    }
    return true;
}

bool json_parse_cstring(arena_t *arena, const char *text, const json_value_t **out_root,
                        const char **out_error, usize *out_error_offset)
{
    if (text == NULL) {
        if (out_error != NULL) {
            *out_error = "文本为空";
        }
        return false;
    }
    return json_parse(arena, text, strlen(text), out_root, out_error, out_error_offset);
}

// ---------------------------------------------------------------- 查询

const json_value_t *json_object_get(const json_value_t *object, const char *key)
{
    if (object == NULL || key == NULL || object->kind != JSON_OBJECT) {
        return NULL;
    }
    for (usize i = 0; i < object->item_count; ++i) {
        if (object->keys[i] != NULL && strcmp(object->keys[i], key) == 0) {
            return object->items[i];
        }
    }
    return NULL;
}

const json_value_t *json_array_at(const json_value_t *array, usize index)
{
    if (array == NULL || array->kind != JSON_ARRAY || index >= array->item_count) {
        return NULL;
    }
    return array->items[index];
}

bool json_as_bool(const json_value_t *v, bool *out)
{
    if (v == NULL || v->kind != JSON_BOOL || out == NULL) {
        return false;
    }
    *out = v->boolean;
    return true;
}

bool json_as_f64(const json_value_t *v, f64 *out)
{
    if (v == NULL || v->kind != JSON_NUMBER || out == NULL) {
        return false;
    }
    *out = v->number;
    return true;
}

bool json_as_f32(const json_value_t *v, f32 *out)
{
    f64 value = 0.0;
    if (!json_as_f64(v, &value) || out == NULL) {
        return false;
    }
    // 判据是"变成 f32 之后是否溢出成 inf"，而不是"这个十进制值是否大于 FLT_MAX"。
    // 后者会误杀 FLT_MAX 自己：FLT_MAX ≈ 3.402823466e38，而它能精确往返的最短
    // 文本是 3.40282347e38 —— 作为 f64 它**大于** FLT_MAX，作为 f32 却正好是 FLT_MAX。
    const f32 narrowed = (f32)value;
    if (!isfinite((double)narrowed)) {
        return false;
    }
    *out = narrowed;
    return true;
}

bool json_as_i32(const json_value_t *v, i32 *out)
{
    f64 value = 0.0;
    if (!json_as_f64(v, &value) || out == NULL) {
        return false;
    }
    if (value != (double)(i32)value) {
        return false; // 带小数或越界
    }
    *out = (i32)value;
    return true;
}

bool json_as_u32(const json_value_t *v, u32 *out)
{
    f64 value = 0.0;
    if (!json_as_f64(v, &value) || out == NULL || value < 0.0) {
        return false;
    }
    if (value != (double)(u32)value) {
        return false;
    }
    *out = (u32)value;
    return true;
}

bool json_as_string(const json_value_t *v, const char **out, usize *out_length)
{
    if (v == NULL || v->kind != JSON_STRING) {
        return false;
    }
    if (out != NULL) {
        *out = v->string;
    }
    if (out_length != NULL) {
        *out_length = v->string_length;
    }
    return true;
}

f32 json_get_f32(const json_value_t *object, const char *key, f32 fallback)
{
    f32 out = fallback;
    (void)json_as_f32(json_object_get(object, key), &out);
    return out;
}

u32 json_get_u32(const json_value_t *object, const char *key, u32 fallback)
{
    u32 out = fallback;
    (void)json_as_u32(json_object_get(object, key), &out);
    return out;
}

i32 json_get_i32(const json_value_t *object, const char *key, i32 fallback)
{
    i32 out = fallback;
    (void)json_as_i32(json_object_get(object, key), &out);
    return out;
}

bool json_get_bool(const json_value_t *object, const char *key, bool fallback)
{
    bool out = fallback;
    (void)json_as_bool(json_object_get(object, key), &out);
    return out;
}

const char *json_get_string(const json_value_t *object, const char *key, const char *fallback)
{
    const json_value_t *v = json_object_get(object, key);
    const char        *s = NULL;
    if (json_as_string(v, &s, NULL)) {
        return s;
    }
    return fallback;
}

// =============================================================================
//  生成

void json_writer_init(json_writer_t *w, bio_writer_t *out, u32 indent_spaces)
{
    if (w == NULL) {
        return;
    }
    memset(w, 0, sizeof(*w));
    w->out = out;
    w->indent_spaces = indent_spaces;
}

bool json_writer_ok(const json_writer_t *w)
{
    return (w != NULL) && !w->failed && (w->out != NULL) && bio_writer_ok(w->out);
}

static bool json_raw(json_writer_t *w, const char *text)
{
    if (w == NULL || w->failed || w->out == NULL) {
        return false;
    }
    return bio_write_bytes(w->out, text, strlen(text));
}

static bool json_char(json_writer_t *w, char c)
{
    if (w == NULL || w->failed || w->out == NULL) {
        return false;
    }
    return bio_write_u8(w->out, (u8)c);
}

static bool json_indent(json_writer_t *w, usize level)
{
    if (w->indent_spaces == 0u) {
        return true;
    }
    if (!json_char(w, '\n')) {
        return false;
    }
    for (usize i = 0; i < level * (usize)w->indent_spaces; ++i) {
        if (!json_char(w, ' ')) {
            return false;
        }
    }
    return true;
}

// 写一个成员之前的分隔。刚写完 key 时不需要（位置已经摆好）。
static bool json_separator(json_writer_t *w)
{
    if (w->pending_key) {
        w->pending_key = false;
        return true;
    }
    const usize level = w->depth;
    if (level >= JSON_MAX_DEPTH) {
        w->failed = true;
        return false;
    }
    if (level > 0u) {
        if (!w->empty[level]) {
            if (!json_char(w, ',')) {
                return false;
            }
        }
        // 这一层是"同一行"的容器（向量的三个分量、八个槽位名）：
        // 只补一个空格，不换行。理由写在 json.h 的字段注释里。
        if (w->inline_at[level]) {
            if (!w->empty[level]) {
                if (!json_char(w, ' ')) {
                    return false;
                }
            }
        } else if (!json_indent(w, level)) {
            // 根值自己不缩进：否则美化输出会以一个多余的空行开头
            return false;
        }
    }
    w->empty[level] = false;
    return true;
}

static bool json_push(json_writer_t *w, char open, bool inline_level)
{
    if (!json_separator(w)) {
        return false;
    }
    if (w->depth >= JSON_MAX_DEPTH) {
        w->failed = true;
        return false;
    }
    if (!json_char(w, open)) {
        return false;
    }
    // 先进入新层，再把**新层**标记为"还没有成员"。
    // 反过来写（先置 empty 再 depth++）会把上一层的"已有成员"标志冲掉，
    // 于是下一个兄弟键前面不再补逗号 —— 产出的是不合法的 JSON，
    // 而且不报错，只在下一次解析时表现为"少字段"。
    w->depth++;
    w->empty[w->depth] = true;
    w->inline_at[w->depth] = inline_level;
    return true;
}

static bool json_pop(json_writer_t *w, char open, char close)
{
    ENGINE_UNUSED(open);
    if (w->depth == 0u) {
        w->failed = true;
        return false;
    }
    w->depth--;
    // 有成员时才需要换行收尾，空容器写成 {} / []；
    // 同一行的容器（json_begin_array_inline）收尾不换行。
    if (!w->empty[w->depth + 1u] && !w->inline_at[w->depth + 1u]) {
        if (!json_indent(w, w->depth)) {
            return false;
        }
    }
    w->empty[w->depth + 1u] = true;
    return json_char(w, close);
}

bool json_begin_object(json_writer_t *w)
{
    return json_push(w, '{', false);
}

bool json_end_object(json_writer_t *w)
{
    return json_pop(w, '{', '}');
}

bool json_begin_array(json_writer_t *w)
{
    return json_push(w, '[', false);
}

bool json_begin_array_inline(json_writer_t *w)
{
    return json_push(w, '[', true);
}

bool json_end_array(json_writer_t *w)
{
    return json_pop(w, '[', ']');
}

static bool json_string_body(json_writer_t *w, const char *text, usize length);

bool json_write_key(json_writer_t *w, const char *key)
{
    if (!json_separator(w)) {
        return false;
    }
    if (key == NULL) {
        w->failed = true;
        return false;
    }
    // 注意用 json_string_body：分隔符上面已经写过一次了
    if (!json_string_body(w, key, strlen(key))) {
        return false;
    }
    if (!json_char(w, ':')) {
        return false;
    }
    if (w->indent_spaces > 0u && !json_char(w, ' ')) {
        return false;
    }
    w->pending_key = true;
    return true;
}

bool json_write_null(json_writer_t *w)
{
    return json_separator(w) && json_raw(w, "null");
}

bool json_write_bool(json_writer_t *w, bool value)
{
    return json_separator(w) && json_raw(w, value ? "true" : "false");
}

// 数字统一走十进制文本。f32 用 %.9g：9 位十进制足以让任意 float 原样读回，
// 这是"存了读、读了再存逐字节一致"这条验收的基础。
static bool json_write_number_text(json_writer_t *w, const char *text)
{
    if (!json_separator(w)) {
        return false;
    }
    return json_raw(w, text);
}

// 数字统一走十进制文本，并且用**最短可往返**精度：
// 从 1 位有效数字往上试，第一个"读回来逐位相同"的就是答案。
// 固定 %.9g 也能精确往返，但会把 0.1f 写成 0.100000001 ——
// 场景文件是要给人看、给人手改的，那种写法会毁掉它的可用性。
static bool json_format_f32_text(f32 value, char *out, usize cap)
{
    for (int precision = 1; precision <= 9; ++precision) {
        const int n = snprintf(out, cap, "%.*g", precision, (double)value);
        if (n <= 0 || (usize)n >= cap) {
            return false;
        }
        char *end = NULL;
        const f32 back = strtof(out, &end);
        if (back == value) {
            return true; // 逐位可往返。NaN 已被调用方挡掉，所以 == 是可靠的
        }
    }
    const int n = snprintf(out, cap, "%.9g", (double)value);
    return n > 0 && (usize)n < cap;
}

static bool json_format_f64_text(f64 value, char *out, usize cap)
{
    for (int precision = 1; precision <= 17; ++precision) {
        const int n = snprintf(out, cap, "%.*g", precision, (double)value);
        if (n <= 0 || (usize)n >= cap) {
            return false;
        }
        char *end = NULL;
        const f64 back = strtod(out, &end);
        if (back == value) {
            return true;
        }
    }
    const int n = snprintf(out, cap, "%.17g", (double)value);
    return n > 0 && (usize)n < cap;
}

bool json_write_f32(json_writer_t *w, f32 value)
{
    // JSON 没有 NaN / Infinity 的字面量。写出去会产出**不合法**的文档，
    // 所以这里拒绝，而不是写出 "nan" 让下游解析器各执一词。
    if (!isfinite((double)value)) {
        if (w != NULL) {
            w->failed = true;
        }
        return false;
    }
    char text[64];
    if (!json_format_f32_text(value, text, sizeof(text))) {
        if (w != NULL) {
            w->failed = true;
        }
        return false;
    }
    return json_write_number_text(w, text);
}

bool json_write_f64(json_writer_t *w, f64 value)
{
    if (!isfinite(value)) {
        if (w != NULL) {
            w->failed = true;
        }
        return false;
    }
    char text[64];
    if (!json_format_f64_text(value, text, sizeof(text))) {
        if (w != NULL) {
            w->failed = true;
        }
        return false;
    }
    return json_write_number_text(w, text);
}

bool json_write_u32(json_writer_t *w, u32 value)
{
    char text[16];
    snprintf(text, sizeof(text), "%u", (unsigned)value);
    return json_write_number_text(w, text);
}

bool json_write_i32(json_writer_t *w, i32 value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", (int)value);
    return json_write_number_text(w, text);
}

// 只写字符串本体（含引号与转义），**不**处理分隔符。
// 单独拆出来是因为 json_write_key 已经处理过分隔符了：
// 让它直接调用 json_write_string_n 会写进两次分隔符，
// 于是每个 key 前面冒出双逗号，产出的是不合法的 JSON。
static bool json_string_body(json_writer_t *w, const char *text, usize length)
{
    if (w == NULL || w->failed || w->out == NULL) {
        return false;
    }
    if (text == NULL) {
        w->failed = true;
        return false;
    }
    if (!json_char(w, '"')) {
        return false;
    }

    for (usize i = 0; i < length; ++i) {
        const u8 c = (u8)text[i];
        switch (c) {
        case '"':
            if (!json_raw(w, "\\\"")) {
                return false;
            }
            break;
        case '\\':
            if (!json_raw(w, "\\\\")) {
                return false;
            }
            break;
        case '\b':
            if (!json_raw(w, "\\b")) {
                return false;
            }
            break;
        case '\f':
            if (!json_raw(w, "\\f")) {
                return false;
            }
            break;
        case '\n':
            if (!json_raw(w, "\\n")) {
                return false;
            }
            break;
        case '\r':
            if (!json_raw(w, "\\r")) {
                return false;
            }
            break;
        case '\t':
            if (!json_raw(w, "\\t")) {
                return false;
            }
            break;
        default:
            // 其余控制字符必须转义（\u00XX），否则产出的不是合法 JSON。
            if (c < 0x20u) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                if (!json_raw(w, esc)) {
                    return false;
                }
            } else if (!json_char(w, (char)c)) {
                return false;
            }
            break;
        }
    }

    return json_char(w, '"');
}

bool json_write_string_n(json_writer_t *w, const char *text, usize length)
{
    if (!json_separator(w)) {
        return false;
    }
    return json_string_body(w, text, length);
}

bool json_write_string(json_writer_t *w, const char *text)
{
    if (text == NULL) {
        if (w != NULL) {
            w->failed = true;
        }
        return false;
    }
    return json_write_string_n(w, text, strlen(text));
}
