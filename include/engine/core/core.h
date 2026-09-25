// =============================================================================
//  engine/core/core.h —— core 模块总入口
//
//  业务代码只需要 #include "engine/core/core.h"。
//  需要更细粒度控制（比如只想用内存层）时才单独包含子头文件。
//
//  包含顺序即依赖顺序：types -> assert/console/log -> memory -> 容器 -> 数学。
//  数学内部也是分层的：math(标量) -> vec -> mat -> quat -> transform。
// =============================================================================
#ifndef ENGINE_CORE_CORE_H
#define ENGINE_CORE_CORE_H

#include "engine/core/types.h"

#include "engine/core/assert.h"
#include "engine/core/console.h"
#include "engine/core/log.h"

#include "engine/core/memory.h"
#include "engine/core/array.h"
#include "engine/core/hash.h"

#include "engine/core/math.h"
#include "engine/core/vec.h"
#include "engine/core/mat.h"
#include "engine/core/quat.h"
#include "engine/core/transform.h"

#endif // ENGINE_CORE_CORE_H
