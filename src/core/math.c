// =============================================================================
//  src/core/math.c —— 标量数学中不适合内联的部分
//
//  为什么这里只有两个函数：
//  绝大多数向量/矩阵运算必须内联才有意义（见 math.h 的说明）。只有这两类
//  例外 —— 一个是位运算循环，一个是修改硬件浮点状态，都不值得内联。
// =============================================================================
#include "engine/core/math.h"

#include "engine/core/assert.h"

// ---------------------------------------------------------------- 浮点环境
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <pmmintrin.h>
#include <xmmintrin.h>
#define MYENGINE_HAS_SSE_FP_ENV 1
#elif defined(__SSE__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define MYENGINE_HAS_SSE_FP_ENV 1
#else
#define MYENGINE_HAS_SSE_FP_ENV 0
#endif

// 只是记录"我们请求过什么"，不代表当前线程真实状态。
// MXCSR 是每线程独立的，所以本函数必须由每个线程自己调用一次。
static bool g_flush_denormals_requested = false;

bool math_get_flush_denormals(void)
{
    return g_flush_denormals_requested;
}

void math_set_flush_denormals(bool enabled)
{
#if MYENGINE_HAS_SSE_FP_ENV
    _MM_SET_FLUSH_ZERO_MODE(enabled ? _MM_FLUSH_ZERO_ON : _MM_FLUSH_ZERO_OFF);
#ifdef _MM_SET_DENORMALS_ZERO_MODE
    _MM_SET_DENORMALS_ZERO_MODE(enabled ? _MM_DENORMALS_ZERO_ON : _MM_DENORMALS_ZERO_OFF);
#endif
    g_flush_denormals_requested = enabled;
#else
    // 非 x86 平台（例如 ARM）默认就没有非规格化数性能问题，安全降级为空操作。
    ENGINE_UNUSED(enabled);
    g_flush_denormals_requested = false;
#endif
}

// ---------------------------------------------------------------- 整数工具
u32 math_next_pow2_u32(u32 v)
{
    if (v == 0) {
        return 1;
    }
    if (v > 0x80000000u) {
        // 无法用 u32 表示更大的 2 的幂，返回 0 让调用方自行处理
        return 0;
    }

    // 把最高位以下全部填 1，再加 1 就得到下一个 2 的幂。
    // 比循环左移快，且没有分支。
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}
