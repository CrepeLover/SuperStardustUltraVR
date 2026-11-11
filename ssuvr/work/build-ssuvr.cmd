@echo off
rem Builds the shadPS4 PC VR emulator from C:\Games\Farpoint-src (branch ssuvr = Farpoint v0.9.6 + US Farpoint + Steam Frame
rem bindings + Super Stardust Ultra VR support). Same toolchain as C:\Games\farpoint-work\build-farpoint.cmd: clang-cl from
rem LLVM 23.1.4, MSVC 14.44 + Windows SDK via vcvars64, VS-bundled CMake/Ninja, HAVE_ENDIAN_H=0. Idle priority.
rem usage: build-ssuvr.cmd [configure|build] [jobs]
setlocal
set "SRC=C:\Games\Farpoint-src"
set "BLD=C:\Games\Farpoint-src\build\win-us"
set "JOBS=%~2"
if "%JOBS%"=="" set "JOBS=8"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Games\SDKs\clang+llvm-23.1.4-x86_64-pc-windows-msvc\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "VULKAN_SDK="
if "%~1"=="configure" (
  cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link -DCMAKE_RC_COMPILER=llvm-rc ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF -DENABLE_OPENXR=ON ^
    -DALSOFT_UPDATE_BUILD_VERSION=OFF -DHAVE_ENDIAN_H=0 ^
    -DPython3_EXECUTABLE=C:\Users\<user>/envs/python-3.13.16/python.exe || exit /b 1
)
start "" /low /b /wait cmake --build "%BLD%" --target shadps4 --parallel %JOBS% || exit /b 1
