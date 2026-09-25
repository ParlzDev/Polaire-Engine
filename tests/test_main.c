// =============================================================================
//  tests/test_main.c —— 测试入口
//
//  每个测试文件的注册函数都在这里显式列出。
//  新增测试文件时忘记加一行，会出现"测试没跑但显示全过"的情况 ——
//  这比测试失败更危险，所以宁愿多写这一行，也不做自动注册的黑魔法。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include "engine/core/log.h"

int main(int argc, char **argv)
{
    // 测试期间日志只保留 ERROR 以上，且不写文件：
    // 日志噪声会淹没测试输出，而测试的失败信息本来就已经足够详细。
    log_init(NULL, LOG_LEVEL_ERROR);

    // M0：基础层
    test_types_register();
    test_math_register();
    test_vec_register();
    test_mat_register();
    test_quat_register();
    test_transform_register();
    test_memory_register();
    test_array_register();
    test_hash_register();

    // M1：平台层
    test_input_register();
    test_time_register();

    // M2：渲染层
    test_render_register();

    // M3：2D 渲染管线
    test_sprite_register();
    test_atlas_register();
    test_batch_register();

    // M4：3D 渲染管线
    test_frustum_register();
    test_pbr_register();
    test_light_register();
    test_render_target_register();
    test_mesh_file_register();
    test_json_register();

    // M5：ECS + 场景
    test_ecs_register();

    const char *filter = (argc > 1) ? argv[1] : NULL;
    const int   result = test_run_all(filter);

    log_shutdown();
    return result;
}
