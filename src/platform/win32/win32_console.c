// =============================================================================
//  src/platform/win32/win32_console.c —— 控制台能力探测（Win32 实现）
//
//  实现 core/console.h 声明的端口。
//  M0 时这个文件放在 src/core/console.c，M1 按"core 声明端口、平台层实现"
//  的规则迁到这里。
// =============================================================================
#include "engine/core/console.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

bool console_enable_ansi(void)
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) {
        // 不是控制台（stdout 被重定向到文件或管道）。
        // 这种情况绝对不能让调用方输出 ANSI 码，否则日志文件里会混进 ESC 字节。
        return false;
    }

    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        return true; // 已经开启
    }

    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
