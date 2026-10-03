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

if "%POLAIRE_CLANG_BIN%"=="" set "POLAIRE_CLANG_BIN=F:\Path\clang+llvm-23.1.1-x86_64-pc-windows-msvc\bin"
if "%POLAIRE_CMAKE_BIN%"=="" set "POLAIRE_CMAKE_BIN=F:\Path\CMake\bin"
if "%POLAIRE_NINJA_BIN%"=="" set "POLAIRE_NINJA_BIN=D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

set "PATH=%POLAIRE_CLANG_BIN%;%POLAIRE_CMAKE_BIN%;%POLAIRE_NINJA_BIN%;%PATH%"

echo Polaire Engine 工具链已就绪:
where clang
where cmake
where ninja

endlocal & set "PATH=%PATH%"
