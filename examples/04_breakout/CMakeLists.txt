# =============================================================================
#  examples/04_breakout/CMakeLists.txt
# =============================================================================

add_executable(04_breakout main.c)

target_link_libraries(04_breakout PRIVATE engine engine_warnings)
