// =============================================================================
//  engine/core/console.h —— 控制台能力探测
//
//  目前只做一件事：让 Windows 控制台接受 ANSI 转义序列。
//  日志模块靠它决定"要不要输出颜色码"——如果目标不是控制台（输出被重定向到
//  文件或管道），我们就不该把 ESC 字节写进用户的日志文件里。
//
//  TODO(M1)：这一层本质是平台调用，等 PAL 建立后整体迁到 platform/win32。
// =============================================================================
#ifndef ENGINE_CORE_CONSOLE_H
#define ENGINE_CORE_CONSOLE_H

#include "engine/core/types.h"

// 尝试启用 ANSI 转义序列处理。
// 返回 true 表示可以安全地输出颜色码。
// 返回 false 表示当前输出目标不支持（重定向、老版本 Windows、非控制台），
// 调用方必须降级为纯文本输出。
bool console_enable_ansi(void);

#endif // ENGINE_CORE_CONSOLE_H
