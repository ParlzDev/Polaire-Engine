// =============================================================================
//  engine/core/byte_io.h —— 带边界检查的小端字节流读写
//
//  ---- 为什么单独有这一层 ----
//
//  引擎里有三处以上需要"把结构体变成一串确定的字节 / 把一串字节变回值"：
//  .mesh 文件、场景二进制存档、M7 的 pak。它们都必须与主机字节序、
//  与编译器的填充规则无关。
//
//  更重要的是**越界**。M4 修过一个坑：调用 GL 查询时给了一个只装得下
//  4 字节的栈变量，驱动的语义却是"写 N 个 int"，于是踩穿栈 —— 崩在了一行
//  和元数据毫无关系的日志语句上，查了很久。教训是通用的：
//  **凡是往调用方给的缓冲区里写，就必须知道缓冲区有多大。**
//  这里的 reader/writer 都带 capacity，越界时置标志位而不是写出去，
//  调用方在末尾统一判一次即可。
//
//  ---- 用法约定 ----
//
//  * writer：先 init，连续 write，最后用 bio_writer_ok() 判断有没有溢出。
//    溢出后**后续写入是空操作**（不再动缓冲区），所以中途不必每步都判。
//  * reader：连续 read，任何一次越界返回 false 且不推进游标；
//    失败后 reader 进入错误态，后续读一律 false。
//  * 一律小端。不做"按主机字节序"的选项 —— 那等于允许格式随平台漂移。
// =============================================================================
#ifndef ENGINE_CORE_BYTE_IO_H
#define ENGINE_CORE_BYTE_IO_H

#include "engine/core/types.h"

// ---------------------------------------------------------------- 写入端
typedef struct bio_writer_t {
    u8   *data;
    usize capacity;
    usize used;
    bool  failed; // 溢出或参数非法；一旦为真就不再写缓冲区
} bio_writer_t;

void bio_writer_init(bio_writer_t *w, void *data, usize capacity);

bool bio_write_u8(bio_writer_t *w, u8 value);
bool bio_write_u16(bio_writer_t *w, u16 value);
bool bio_write_u32(bio_writer_t *w, u32 value);
bool bio_write_u64(bio_writer_t *w, u64 value);
bool bio_write_i32(bio_writer_t *w, i32 value);
bool bio_write_f32(bio_writer_t *w, f32 value);
bool bio_write_bytes(bio_writer_t *w, const void *src, usize size);
// 直接补 n 个零字节（对齐/保留区用）。
bool bio_write_zeros(bio_writer_t *w, usize n);

// 到目前为止**实际写入**的字节数。溢出时不再推进，所以这个值永远
// 不超过 capacity —— 失败原因看 bio_writer_ok()，不要靠 used 反推。
usize bio_writer_used(const bio_writer_t *w);
bool  bio_writer_ok(const bio_writer_t *w);

// ---------------------------------------------------------------- 读取端
typedef struct bio_reader_t {
    const u8 *data;
    usize     size;
    usize     pos;
    bool      failed;
} bio_reader_t;

void bio_reader_init(bio_reader_t *r, const void *data, usize size);

bool bio_read_u8(bio_reader_t *r, u8 *out);
bool bio_read_u16(bio_reader_t *r, u16 *out);
bool bio_read_u32(bio_reader_t *r, u32 *out);
bool bio_read_u64(bio_reader_t *r, u64 *out);
bool bio_read_i32(bio_reader_t *r, i32 *out);
bool bio_read_f32(bio_reader_t *r, f32 *out);
// 拷贝到 dst（用于结构体字段，避免把未对齐的指针直接交给调用方）。
bool bio_read_bytes(bio_reader_t *r, void *dst, usize size);
// 跳过 n 字节（保留区、不认识的可选字段 —— 向前兼容的关键）。
bool bio_skip(bio_reader_t *r, usize n);

usize bio_reader_remaining(const bio_reader_t *r);
usize bio_reader_pos(const bio_reader_t *r);
bool  bio_reader_ok(const bio_reader_t *r);

#endif // ENGINE_CORE_BYTE_IO_H
