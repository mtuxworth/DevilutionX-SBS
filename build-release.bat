@echo off
rem Release build for distribution: full optimisation with LTO, C runtime linked into the exe, networking on.
rem Needs Visual Studio 2022 or later with the C++ desktop workload and "C++ CMake tools for Windows".
rem Builds in %LOCALAPPDATA% by default (about 1.5 GB); set RELEASE_BUILD_DIR to use another folder.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio with C++ tools was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if "%RELEASE_BUILD_DIR%"=="" set "RELEASE_BUILD_DIR=%LOCALAPPDATA%\devilutionx-sbs\build-release"
cd /d "%~dp0"
if not exist "%RELEASE_BUILD_DIR%\CMakeCache.txt" (
  cmake -S . -B "%RELEASE_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
    -DDISCORD_INTEGRATION=OFF -DNONET=OFF ^
    -DDEVILUTIONX_SYSTEM_ZLIB=OFF -DDEVILUTIONX_SYSTEM_BZIP2=OFF -DDEVILUTIONX_SYSTEM_LUA=OFF ^
    -DDEVILUTIONX_SYSTEM_SDL2=OFF -DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF -DDEVILUTIONX_SYSTEM_SDL_AUDIOLIB=OFF ^
    -DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF -DDEVILUTIONX_SYSTEM_SHEENBIDI=OFF ^
    -DDEVILUTIONX_SYSTEM_UNORDERED_DENSE=OFF -DDEVILUTIONX_SYSTEM_MAGIC_ENUM=OFF ^
    -DDEVILUTIONX_SYSTEM_LIBFMT=OFF -DDEVILUTIONX_SYSTEM_LIBPNG=OFF || exit /b 1
)
cmake --build "%RELEASE_BUILD_DIR%" -j %NUMBER_OF_PROCESSORS% || exit /b 1
