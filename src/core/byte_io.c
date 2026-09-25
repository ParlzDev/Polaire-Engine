// =============================================================================
//  src/core/byte_io.c —— 带边界检查的小端字节流读写
//
//  读写的具体做法：逐字节拼/拆，不做强转。
//  强转 (*(const u32 *)ptr) 有两个问题：要求指针对齐（缓冲区是按偏移切的，
//  编译器有权假设它不对齐），并且违反严格别名规则。逐字节拼拆让编译器
//  自己决定用什么指令，两个问题都不存在。
// =============================================================================
#include "engine/core/byte_io.h"

#include <string.h>

// ---------------------------------------------------------------- 公共
static bool bio_take(usize need, usize size, usize pos)
{
    // 一次判断同时覆盖"溢出"与"pos + need 本身溢出"两种情况。
    if (need > size - pos) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 写入端
void bio_writer_init(bio_writer_t *w, void *data, usize capacity)
{
    if (w == NULL) {
        return;
    }
    w->data = (u8 *)data;
    w->capacity = capacity;
    w->used = 0u;
    w->failed = (data == NULL && capacity > 0u);
}

static u8 *bio_reserve(bio_writer_t *w, usize n)
{
    if (w == NULL || w->failed) {
        return NULL; // 已经坏了就别再动缓冲区
    }
    if (!bio_take(n, w->capacity, w->used)) {
        w->failed = true;
        return NULL;
    }
    u8 *at = w->data + w->used;
    w->used += n;
    return at;
}

bool bio_write_u8(bio_writer_t *w, u8 value)
{
    u8 *at = bio_reserve(w, 1u);
    if (at == NULL) {
        return false;
    }
    at[0] = value;
    return true;
}

bool bio_write_u16(bio_writer_t *w, u16 value)
{
    u8 *at = bio_reserve(w, 2u);
    if (at == NULL) {
        return false;
    }
    at[0] = (u8)(value & 0xFFu);
    at[1] = (u8)((value >> 8) & 0xFFu);
    return true;
}

bool bio_write_u32(bio_writer_t *w, u32 value)
{
    u8 *at = bio_reserve(w, 4u);
    if (at == NULL) {
        return false;
    }
    at[0] = (u8)(value & 0xFFu);
    at[1] = (u8)((value >> 8) & 0xFFu);
    at[2] = (u8)((value >> 16) & 0xFFu);
    at[3] = (u8)((value >> 24) & 0xFFu);
    return true;
}

bool bio_write_u64(bio_writer_t *w, u64 value)
{
    u8 *at = bio_reserve(w, 8u);
    if (at == NULL) {
        return false;
    }
    for (u32 i = 0; i < 8u; ++i) {
        at[i] = (u8)((value >> (i * 8u)) & 0xFFu);
    }
    return true;
}

bool bio_write_i32(bio_writer_t *w, i32 value)
{
    // 有符号数先转成同宽无符号再按位写：避免实现对负数的表示方式做假设
    //（实际上所有目标平台都是补码，但这里没有任何理由依赖它）。
    return bio_write_u32(w, (u32)value);
}

bool bio_write_f32(bio_writer_t *w, f32 value)
{
    u32 bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bio_write_u32(w, bits);
}

bool bio_write_bytes(bio_writer_t *w, const void *src, usize size)
{
    if (src == NULL && size > 0u) {
        if (w != NULL) {
            w->failed = true;
        }
        return false;
    }

    u8 *at = bio_reserve(w, size);
    if (at == NULL) {
        return false;
    }
    if (size > 0u) {
        memcpy(at, src, size);
    }
    return true;
}

bool bio_write_zeros(bio_writer_t *w, usize n)
{
    u8 *at = bio_reserve(w, n);
    if (at == NULL) {
        return false;
    }
    memset(at, 0, n);
    return true;
}

usize bio_writer_used(const bio_writer_t *w)
{
    return (w != NULL) ? w->used : 0u;
}

bool bio_writer_ok(const bio_writer_t *w)
{
    return (w != NULL) && !w->failed;
}

// ---------------------------------------------------------------- 读取端
void bio_reader_init(bio_reader_t *r, const void *data, usize size)
{
    if (r == NULL) {
        return;
    }
    r->data = (const u8 *)data;
    r->size = size;
    r->pos = 0u;
    r->failed = (data == NULL && size > 0u);
}

static const u8 *bio_consume(bio_reader_t *r, usize n)
{
    if (r == NULL || r->failed) {
        return NULL;
    }
    if (!bio_take(n, r->size, r->pos)) {
        r->failed = true;
        return NULL;
    }
    const u8 *at = r->data + r->pos;
    r->pos += n;
    return at;
}

bool bio_read_u8(bio_reader_t *r, u8 *out)
{
    const u8 *at = bio_consume(r, 1u);
    if (at == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = at[0];
    }
    return true;
}

bool bio_read_u16(bio_reader_t *r, u16 *out)
{
    const u8 *at = bio_consume(r, 2u);
    if (at == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = (u16)((u16)at[0] | ((u16)at[1] << 8));
    }
    return true;
}

bool bio_read_u32(bio_reader_t *r, u32 *out)
{
    const u8 *at = bio_consume(r, 4u);
    if (at == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = (u32)at[0] | ((u32)at[1] << 8) | ((u32)at[2] << 16) | ((u32)at[3] << 24);
    }
    return true;
}

bool bio_read_u64(bio_reader_t *r, u64 *out)
{
    const u8 *at = bio_consume(r, 8u);
    if (at == NULL) {
        return false;
    }
    u64 value = 0u;
    for (u32 i = 0; i < 8u; ++i) {
        value |= (u64)at[i] << (i * 8u);
    }
    if (out != NULL) {
        *out = value;
    }
    return true;
}

bool bio_read_i32(bio_reader_t *r, i32 *out)
{
    u32 bits = 0u;
    if (!bio_read_u32(r, &bits)) {
        return false;
    }
    if (out != NULL) {
        *out = (i32)bits;
    }
    return true;
}

bool bio_read_f32(bio_reader_t *r, f32 *out)
{
    u32 bits = 0u;
    if (!bio_read_u32(r, &bits)) {
        return false;
    }
    if (out != NULL) {
        f32 value = 0.0f;
        memcpy(&value, &bits, sizeof(value));
        *out = value;
    }
    return true;
}

bool bio_read_bytes(bio_reader_t *r, void *dst, usize size)
{
    const u8 *at = bio_consume(r, size);
    if (at == NULL) {
        return false;
    }
    if (dst != NULL && size > 0u) {
        memcpy(dst, at, size);
    }
    return true;
}

bool bio_skip(bio_reader_t *r, usize n)
{
    return bio_consume(r, n) != NULL;
}

usize bio_reader_remaining(const bio_reader_t *r)
{
    if (r == NULL || r->failed) {
        return 0u;
    }
    return r->size - r->pos;
}

usize bio_reader_pos(const bio_reader_t *r)
{
    return (r != NULL) ? r->pos : 0u;
}

bool bio_reader_ok(const bio_reader_t *r)
{
    return (r != NULL) && !r->failed;
}
