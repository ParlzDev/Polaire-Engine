# =============================================================================
#  examples/01_window/CMakeLists.txt
# =============================================================================

add_executable(01_window main.c)

target_link_libraries(01_window PRIVATE engine engine_warnings)
