// =============================================================================
//  src/platform/win32/win32_thread.c —— 线程原语（Win32 实现）
//
//  实现 core/thread.h 声明的端口。
// =============================================================================
#include "engine/core/thread.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "engine/core/assert.h"

// 存储必须放得下平台的互斥量实现。写不下会直接编译失败，
// 而不是运行期把栈写坏 —— 这就是把大小做成编译期常量的意义。
_Static_assert(sizeof(CRITICAL_SECTION) <= PLATFORM_MUTEX_STORAGE_SIZE,
               "platform_mutex_t 的存储放不下 CRITICAL_SECTION，请调大 PLATFORM_MUTEX_STORAGE_SIZE");
_Static_assert(alignof(CRITICAL_SECTION) <= PLATFORM_MUTEX_STORAGE_ALIGN,
               "CRITICAL_SECTION 的对齐要求超过了 PLATFORM_MUTEX_STORAGE_ALIGN");

// 自旋次数。临界区很短（日志写入）时，先自旋几百次再进内核能省掉
// 大量用户态/内核态切换。4000 是 Windows 文档推荐的通用值。
#define WIN32_MUTEX_SPIN_COUNT 4000

static CRITICAL_SECTION *win32_mutex(platform_mutex_t *mutex)
{
    return (CRITICAL_SECTION *)(void *)mutex->storage;
}

bool platform_mutex_init(platform_mutex_t *mutex)
{
    ASSERT(mutex != NULL);

    // 用 InitializeCriticalSectionAndSpinCount 而不是 InitializeCriticalSection：
    // 后者在内存不足时会抛出 SEH 异常，而本引擎的错误处理约定是返回错误码。
    return InitializeCriticalSectionAndSpinCount(win32_mutex(mutex), WIN32_MUTEX_SPIN_COUNT) != 0;
}

void platform_mutex_destroy(platform_mutex_t *mutex)
{
    ASSERT(mutex != NULL);
    DeleteCriticalSection(win32_mutex(mutex));
}

void platform_mutex_lock(platform_mutex_t *mutex)
{
    ASSERT(mutex != NULL);
    EnterCriticalSection(win32_mutex(mutex));
}

void platform_mutex_unlock(platform_mutex_t *mutex)
{
    ASSERT(mutex != NULL);
    LeaveCriticalSection(win32_mutex(mutex));
}
