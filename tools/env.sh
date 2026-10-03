#!/usr/bin/env bash
# =============================================================================
#  把本机工具链加进当前 shell 的 PATH（不修改系统环境变量）
#
#  用法：
#      source tools/env.sh
#      cmake --preset debug && cmake --build --preset debug
#
#  路径可用环境变量覆盖，换机器时不必改脚本：
#      export POLAIRE_CLANG_BIN=/path/to/llvm/bin
#      export POLAIRE_CMAKE_BIN=/path/to/cmake/bin
#      export POLAIRE_NINJA_BIN=/path/to/ninja
#
#  注意 1：本机 clang 是 MSVC 目标（x86_64-pc-windows-msvc），它靠自动探测找到
#  Visual Studio 的 CRT 头与库。因此 Visual Studio 必须安装（本机在 D 盘）。
#
#  注意 2（踩过一次的坑）：往 PATH 里塞 Windows 风格路径 "D:/foo/bar" 会被
#  MSYS/Git Bash 的路径列表转换逻辑从冒号处截断，变成两段 "D" 和 "/foo/bar"，
#  结果就是"目录明明存在、工具却找不到"。必须先转成 /d/foo/bar。
# =============================================================================

POLAIRE_CLANG_BIN="${POLAIRE_CLANG_BIN:-F:/Path/clang+llvm-23.1.1-x86_64-pc-windows-msvc/bin}"
POLAIRE_CMAKE_BIN="${POLAIRE_CMAKE_BIN:-F:/Path/CMake/bin}"
POLAIRE_NINJA_BIN="${POLAIRE_NINJA_BIN:-D:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja}"

# D:/foo  ->  /d/foo
_polaire_to_posix() {
    case "$1" in
        [A-Za-z]:/*)
            local drive
            drive=$(printf '%s' "${1:0:1}" | tr 'A-Z' 'a-z')
            printf '/%s%s' "$drive" "${1:2}"
            ;;
        *)
            printf '%s' "$1"
            ;;
    esac
}

_polaire_prepend() {
    local dir
    dir=$(_polaire_to_posix "$1")
    if [ -d "$dir" ]; then
        case ":$PATH:" in
            *":$dir:"*) ;;
            *) PATH="$dir:$PATH" ;;
        esac
    else
        printf 'env.sh: 目录不存在，已跳过 -> %s\n' "$dir" >&2
    fi
}

_polaire_prepend "$POLAIRE_CLANG_BIN"
_polaire_prepend "$POLAIRE_CMAKE_BIN"
_polaire_prepend "$POLAIRE_NINJA_BIN"
export PATH

unset -f _polaire_prepend _polaire_to_posix

echo "Polaire Engine 工具链已就绪:"
printf '  clang : %s\n' "$(command -v clang || echo '未找到')"
printf '  cmake : %s\n' "$(command -v cmake || echo '未找到')"
printf '  ninja : %s\n' "$(command -v ninja || echo '未找到')"
