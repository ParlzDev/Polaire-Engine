// =============================================================================
//  tests/test_suites.h —— 各测试文件的注册函数声明
//
//  每个测试文件包含本头文件，让自己定义的 test_xxx_register 有可见的原型
//  （否则 -Wmissing-prototypes 会告警）；test_main.c 也包含它来调用全部注册函数。
//
//  新增测试文件时：在这里加一行声明，并在 test_main.c 里加一行调用。
//  忘了加不会报错，但会变成"测试根本没跑"——所以这个文件要当清单看。
// =============================================================================
#ifndef MYENGINE_TEST_SUITES_H
#define MYENGINE_TEST_SUITES_H

// M0：基础层
void test_types_register(void);
void test_math_register(void);
void test_vec_register(void);
void test_mat_register(void);
void test_quat_register(void);
void test_transform_register(void);
void test_memory_register(void);
void test_array_register(void);
void test_hash_register(void);

// M1：平台层
void test_input_register(void);
void test_time_register(void);

// M2：渲染层
void test_render_register(void);

// M3：2D 渲染管线
void test_sprite_register(void);
void test_atlas_register(void);
void test_batch_register(void);

// M4：3D 渲染管线
void test_frustum_register(void);
void test_pbr_register(void);
void test_light_register(void);
void test_render_target_register(void);
void test_mesh_file_register(void);
void test_json_register(void);

#endif // MYENGINE_TEST_SUITES_H
