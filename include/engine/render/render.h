// =============================================================================
//  engine/render/render.h —— 渲染层总入口
//
//  业务代码通常只需要包含 engine/engine.h，它会把这个文件带进来。
//
//  分层（从下到上）：
//    render_types    后端无关的类型与预设
//    gl_types/gl_api GL 的类型与函数表（只有 GL 后端实现需要）
//    render_backend  后端抽象接口（按 Vulkan 语义）
//    gl_context      GL 上下文
//    camera / render_queue / uniforms   渲染组织
//    shader / texture / mesh / material 资源
//    obj_loader      模型加载
//    renderer        引擎级外观（业务代码只跟它打交道）
// =============================================================================
#ifndef ENGINE_RENDER_RENDER_H
#define ENGINE_RENDER_RENDER_H

// 后端无关的类型与接口
#include "engine/render/render_types.h"
#include "engine/render/uniforms.h"
#include "engine/render/render_backend.h"

// GL 后端（类型与函数表；具体实现文件在 src/render/backend/opengl/）
#include "engine/render/gl_types.h"
#include "engine/render/gl_api.h"
#include "engine/render/gl_context.h"
#include "engine/render/backend/opengl/gl_backend.h"

// 渲染组织
#include "engine/render/camera.h"
#include "engine/render/camera_2d.h"
#include "engine/render/render_queue.h"

// 资源
#include "engine/render/shader.h"
#include "engine/render/texture.h"
#include "engine/render/texture_atlas.h"
#include "engine/render/mesh.h"
#include "engine/render/material.h"
#include "engine/render/obj_loader.h"
#include "engine/render/mesh_file.h"

// 2D 管线（M3）
#include "engine/render/sprite.h"
#include "engine/render/sprite_batch.h"
#include "engine/render/debug_draw.h"
#include "engine/render/font.h"
#include "engine/render/text.h"
#include "engine/render/tilemap.h"

// 3D 管线（M4）
#include "engine/render/frustum.h"
#include "engine/render/light.h"
#include "engine/render/pbr.h"
#include "engine/render/render_target.h"
#include "engine/render/shadow.h"

// 引擎级外观
#include "engine/render/renderer.h"

#endif // ENGINE_RENDER_RENDER_H
