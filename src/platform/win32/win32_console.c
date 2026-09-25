// =============================================================================
//  src/platform/win32/win32_console.c —— 控制台能力探测（Win32 实现）
//
//  实现 core/console.h 声明的端口。
//  M0 时这个文件放在 src/core/console.c，M1 按"core 声明端口、平台层实现"
//  的规则迁到这里。
// =============================================================================
#include "engine/core/console.h"

#include <stdlib.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

bool console_enable_ansi(void)
{
    // NO_COLOR（no-color.org）是个约定：环境里只要设了它，就说明用户的终端
    // 不想要 —— 或不能 —— 解释转义序列。IDE 的输出面板就是典型：它把子进程的
    // stdout 当纯文本收下来，VT 码于是以"[90m"这种字面形态显示出来，
    // 比乱码更难读。探测不出这种代理情形，所以给一个显式开关。
    const char *no_color = getenv("NO_COLOR");
    if (no_color != NULL && no_color[0] != '\0') {
        return false;
    }

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

bool console_set_utf8(void)
{
    // 中文 Windows 的控制台默认用系统 ANSI 代码页（936/GBK）。引擎写出去的一切
    // 都是 UTF-8 字节，不换代码页就会被按 GBK 两两配对，日志里的中文变成乱码 ——
    // 而乱掉的日志比没有日志更糟：报错就在那一行，人读不出来。
    //
    // 两个都要设：OutputCP 管日志与 printf 的**显示**，InputCP 管键盘输入
    // （M11 的控制台命令、以及任何读 stdin 的路径）。
    //
    // 注意这只影响本进程持有的控制台，不改系统区域设置，也不影响重定向到文件
    // 的字节 —— 那些字节本来就是 UTF-8。
    const bool out_ok = (SetConsoleOutputCP(CP_UTF8) != 0);
    const bool in_ok = (SetConsoleCP(CP_UTF8) != 0);
    return out_ok && in_ok;
}
