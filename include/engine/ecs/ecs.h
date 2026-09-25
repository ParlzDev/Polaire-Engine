// =============================================================================
//  engine/ecs/ecs.h —— ECS 层的伞形头
//
//  包含顺序 = 依赖顺序：entity → component → query/system/transform → world。
//  和 core/platform/render 三层的伞形头同一个约定：应用写一份场景时不必记住
//  六个文件各自管什么，只需要知道"ecs 层在这里"。
//
//  刻意**不含**任何 render 头（M5 约束 3）。ECS 层要能被物理、脚本、
//  编辑器头表复用，不能拖着 GL 走 —— 同时看得见两边的桥接在 scene/ecs_submit.h。
// =============================================================================
#ifndef ENGINE_ECS_H
#define ENGINE_ECS_H

#include "engine/ecs/component.h"
#include "engine/ecs/entity.h"
#include "engine/ecs/query.h"
#include "engine/ecs/system.h"
#include "engine/ecs/transform.h"
#include "engine/ecs/world.h"

#endif // ENGINE_ECS_H
