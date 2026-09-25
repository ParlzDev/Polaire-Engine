@echo off
REM ============================================================================
REM  把本机工具链加进当前 cmd 会话的 PATH（不修改系统环境变量）
REM
REM  用法：
REM      tools\env.cmd
REM      cmake --preset debug
REM
REM  本机 clang 为 MSVC 目标，需要已安装 Visual Studio 提供 CRT。
REM ============================================================================
setlocal

if "%MYENGINE_CLANG_BIN%"=="" set "MYENGINE_CLANG_BIN=F:\Path\clang+llvm-23.1.1-x86_64-pc-windows-msvc\bin"
if "%MYENGINE_CMAKE_BIN%"=="" set "MYENGINE_CMAKE_BIN=F:\Path\CMake\bin"
if "%MYENGINE_NINJA_BIN%"=="" set "MYENGINE_NINJA_BIN=D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

set "PATH=%MYENGINE_CLANG_BIN%;%MYENGINE_CMAKE_BIN%;%MYENGINE_NINJA_BIN%;%PATH%"

echo MyEngine 工具链已就绪:
where clang
where cmake
where ninja

endlocal & set "PATH=%PATH%"
