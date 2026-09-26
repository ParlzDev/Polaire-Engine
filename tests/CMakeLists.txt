# =============================================================================
#  tests/CMakeLists.txt
# =============================================================================

add_executable(engine_tests
    test_framework.c
    test_main.c
    test_types.c
    test_math.c
    test_vec.c
    test_mat.c
    test_quat.c
    test_transform.c
    test_memory.c
    test_array.c
    test_hash.c
    test_input.c
    test_time.c
    test_render.c
    test_sprite.c
    test_atlas.c
    test_tilemap.c
    test_batch.c
    test_frustum.c
    test_pbr.c
    test_light.c
    test_render_target.c
    test_mesh_file.c
    test_json.c
    test_ecs.c
    test_scene_io.c
    test_prefab.c
)

target_include_directories(engine_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(engine_tests PRIVATE engine engine_warnings)

# 注册到 CTest。命令行传参是筛选条件，例如：
#     ctest --preset debug -- -R math
add_test(NAME engine_tests COMMAND engine_tests)

# 让 ctest 输出里也能看到测试自己的彩色摘要
set_tests_properties(engine_tests PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "CTEST_OUTPUT_ON_FAILURE=1"
)
