// =============================================================================
//  engine/core/types.h —— 基础类型、通用宏、句柄
//
//  设计要点：
//   * 固定宽度别名（u8/i32/f32...），禁止裸 int/long —— 它们的宽度随平台变，
//     而引擎的序列化格式、GPU 布局、句柄位域都依赖确定的宽度。
//   * 所有"结构体大小/对齐"约定用 _Static_assert 固化在编译期。
//     运行期踩到内存布局的坑代价是几天调试；编译期报错代价是 5 秒。
//   * handle_t 是 32 位打包值（24 位索引 + 8 位代际），不是指针。
//     上层永远拿不到裸指针，因此"资源卸载后旧句柄误用"不会变成野指针，
//     而会变成一次可检出的 generation 不匹配。
// =============================================================================
#ifndef ENGINE_CORE_TYPES_H
#define ENGINE_CORE_TYPES_H

#include <stdalign.h> // C11: _Alignas / alignof
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- 整数/浮点
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float  f32;
typedef double f64;

typedef size_t    usize;
typedef ptrdiff_t isize;

typedef bool b32;
typedef u8   b8;

// 引擎只支持 64 位目标：句柄位域、指针比较、SIMD 对齐均以此为前提。
// 32 位下 handle_t 的打包策略需要重新设计，与其静默跑错不如直接拦住。
_Static_assert(sizeof(void *) == 8, "MyEngine 只支持 64 位目标平台");

// ---------------------------------------------------------------- 常用宏
#define ENGINE_UNUSED(x) ((void)(x))

#define ENGINE_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define ENGINE_KB(n) ((usize)(n) * 1024u)
#define ENGINE_MB(n) (ENGINE_KB(n) * 1024u)
#define ENGINE_GB(n) (ENGINE_MB(n) * 1024u)

// 要求 a 为 2 的幂。断言在 Debug 下生效，见 core/assert.h
#define ENGINE_ALIGN_UP(v, a) (((usize)(v) + ((usize)(a) - 1u)) & ~((usize)(a) - 1u))
#define ENGINE_IS_POW2(x) (((x) != 0) && (((x) & ((x) - 1u)) == 0))

#define ENGINE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define ENGINE_MAX(a, b) ((a) > (b) ? (a) : (b))

#define ENGINE_SWAP(T, a, b)                                                                       \
    do {                                                                                           \
        T _engine_swap_tmp = (a);                                                                  \
        (a)                = (b);                                                                  \
        (b)                = _engine_swap_tmp;                                                     \
    } while (0)

#define ENGINE_OFFSETOF(T, member) offsetof(T, member)
#define ENGINE_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)

// 让编译器的 printf 格式串检查作用于我们的日志函数。
// 没有这个属性，`log_printf("%d", some_u64)` 这类错误要到运行期才炸。
#if defined(__clang__) || defined(__GNUC__)
#define ENGINE_PRINTF_FMT(fmt_index, first_arg_index)                                              \
    __attribute__((format(printf, fmt_index, first_arg_index)))
#else
#define ENGINE_PRINTF_FMT(fmt_index, first_arg_index)
#endif

// =============================================================================
//  句柄
//
//  布局（小端序视角，位 31..0）：
//
//     31            24 23                          0
//     +---------------+-----------------------------+
//     |  generation   |           index             |
//     +---------------+-----------------------------+
//
//  * index 是槽位下标，0 号槽永久保留，因此 handle_make(0, 0) == HANDLE_INVALID。
//  * generation 在槽位释放时自增，使旧句柄立刻失效（防 ABA 悬空）。
//  * 24 位索引 = 最多 16,777,215 个并发对象；8 位代际 = 同一槽位可复用 255 次。
//    超过 255 次复用后代际回绕，理论上存在 ABA 风险。这是 4 字节句柄的固有代价，
//    换成 {u32 index, u32 generation} 的 8 字节句柄可以消除，但代价是所有
//    句柄容器翻倍。M0 选择 4 字节，在 docs/M0-设计决策.md 中有完整取舍说明。
// =============================================================================
typedef u32 handle_t;

#define HANDLE_INDEX_BITS 24u
#define HANDLE_GEN_BITS 8u
#define HANDLE_INDEX_MASK ((u32)0x00FFFFFFu)
#define HANDLE_GEN_MASK ((u32)0xFF000000u)
#define HANDLE_MAX_INDEX ((u32)0x00FFFFFFu)
#define HANDLE_INVALID ((handle_t)0u)

static inline handle_t handle_make(u32 index, u8 generation)
{
    return (handle_t)((index & HANDLE_INDEX_MASK) | ((u32)generation << HANDLE_INDEX_BITS));
}

static inline u32 handle_index(handle_t h)
{
    return (u32)(h & HANDLE_INDEX_MASK);
}

static inline u8 handle_generation(handle_t h)
{
    return (u8)((h & HANDLE_GEN_MASK) >> HANDLE_INDEX_BITS);
}

static inline bool handle_is_valid(handle_t h)
{
    return h != HANDLE_INVALID;
}

// 显式命名比 `a == b` 更能表达意图（比较的是"身份"，不是"数值"）
static inline bool handle_equal(handle_t a, handle_t b)
{
    return a == b;
}

#endif // ENGINE_CORE_TYPES_H
