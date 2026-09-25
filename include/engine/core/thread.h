// =============================================================================
//  engine/core/thread.h —— 线程原语（端口声明）
//
//  这是一个"端口"（port）：**接口声明在 core 层，实现在平台层**
//  （src/platform/win32/win32_thread.c）。这样 core 层可以需要互斥量，
//  却不必反向依赖平台层，也不会把 windows.h 拖进核心模块。
//
//  同样的模式也用在 core/console.h 上。
//
//  为什么不直接前向声明一个不透明类型（`typedef struct mutex_t mutex_t;`）：
//  调用方（日志模块）需要把互斥量作为结构体成员**内嵌**，而不是再分配一次。
//  内嵌要求编译期知道大小，所以这里用定长字节缓冲 + 对齐。
//  代价是存储略大于实际需要，收益是零额外分配、零指针间接。
// =============================================================================
#ifndef ENGINE_CORE_THREAD_H
#define ENGINE_CORE_THREAD_H

#include "engine/core/types.h"

// 64 字节 / 16 字节对齐足以容纳各平台的互斥量实现
// （Win32 CRITICAL_SECTION 在 x64 上是 40 字节；pthread_mutex_t 在 Linux 上是 40）。
// 平台实现文件里有 _Static_assert 校验，写不下会直接编译失败。
#define PLATFORM_MUTEX_STORAGE_SIZE 64u
#define PLATFORM_MUTEX_STORAGE_ALIGN 16u

typedef struct platform_mutex_t {
    _Alignas(PLATFORM_MUTEX_STORAGE_ALIGN) u8 storage[PLATFORM_MUTEX_STORAGE_SIZE];
} platform_mutex_t;

_Static_assert(sizeof(platform_mutex_t) == PLATFORM_MUTEX_STORAGE_SIZE,
               "platform_mutex_t 必须是纯存储，不能有额外成员");

// 初始化。失败返回 false（实际只在极端资源不足时发生）。
bool platform_mutex_init(platform_mutex_t *mutex);

// 销毁。销毁后再使用是未定义行为。
void platform_mutex_destroy(platform_mutex_t *mutex);

// 不可重入：同一线程重复 lock 会死锁。
// 需要重入请用 platform_mutex_t 之外的方案（例如递归锁），本引擎不需要。
void platform_mutex_lock(platform_mutex_t *mutex);
void platform_mutex_unlock(platform_mutex_t *mutex);

#endif // ENGINE_CORE_THREAD_H
