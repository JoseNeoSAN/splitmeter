@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem 优先使用环境变量 CXX 指定的编译器，其次找项目自带的便携工具链
set "GXX=%CXX%"
if "%GXX%"=="" (
  if exist "..\tools\mingw64\bin\g++.exe" set "GXX=..\tools\mingw64\bin\g++.exe"
)
if "%GXX%"=="" (
  where g++ >nul 2>nul && set "GXX=g++"
)
if "%GXX%"=="" (
  echo [错误] 找不到 g++。请安装 MinGW-w64，或用 set CXX=^<路径^> 指定。
  exit /b 1
)

for %%I in ("%GXX%") do set "BIN=%%~dpI"
set "WINDRES=%BIN%windres.exe"

if not exist "build" mkdir build

echo [1/3] 编译资源...
"%WINDRES%" -i res\app.rc -o build\app_res.o || exit /b 1

echo [2/3] 编译并链接...
"%GXX%" -std=c++17 -O2 -s -municode -mwindows -static -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  -fno-exceptions -fno-rtti ^
  src\core.cpp src\main.cpp build\app_res.o ^
  -o build\SplitMeter.exe ^
  -lgdiplus -lshell32 -luser32 -lgdi32 -ladvapi32 -lole32 -luuid || exit /b 1

echo [3/3] 完成: build\SplitMeter.exe
for %%F in (build\SplitMeter.exe) do echo      体积: %%~zF 字节
