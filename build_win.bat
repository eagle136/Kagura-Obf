@echo off
REM ===========================================================================
REM Build KaguraObfuscator for LLVM 17.0.6 on Windows (MSVC + Ninja)
REM
REM Fixed 2026-11-05. The original script had three bugs that produce the
REM wrong artifact if copied verbatim:
REM   1) LLVM_DIR pointed at D:\llvm-install, which is LLVM 22.1.6, so it
REM      emitted a 22.x plugin instead of 17.x. The correct 17.0.6 dev package
REM      is C:\llvm17 (ships include/ + libs + CMake config).
REM   2) Must use MSVC cl.exe (provided by vcvars64), not the clang-cl bundled
REM      with llvm17: the VS18 MSVC STL calls __builtin_verbose_trap(), which
REM      clang 17 does not know, and the compile fails hard.
REM   3) Must explicitly point at the Windows SDK mt.exe: llvm17's llvm-mt.exe
REM      lacks libxml2 and fails at link time.
REM
REM Artifact: build\lib\Transforms\KaguraObfuscator.dll
REM           copy to C:\kagura\KaguraObfuscator17.dll when deploying.
REM ===========================================================================

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [!] vcvars64 failed & exit /b 1 )

cd /d D:\tmp\kagura-main
if not exist build mkdir build

REM Windows SDK version varies per machine: pick the highest one that has mt.exe
set "SDKBIN=C:\Program Files (x86)\Windows Kits\10\bin"
set "SDKVER="
for /f "delims=" %%d in ('dir /b /ad /o-n "%SDKBIN%" 2^>nul') do (
    if exist "%SDKBIN%\%%d\x64\mt.exe" (
        set "SDKVER=%%d"
        goto :sdkfound
    )
)
:sdkfound
if not defined SDKVER ( echo [!] Windows SDK mt.exe not found & exit /b 1 )
echo [+] Windows SDK: %SDKVER%

cmake -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MT="%SDKBIN%\%SDKVER%\x64\mt.exe" ^
  -DLLVM_DIR=C:/llvm17/lib/cmake/llvm ^
  -DKAGURA_BUILD_TESTS=OFF ^
  -DKAGURA_BITCODE_TOOLS=OFF ^
  -DKAGURA_LEGACY_PM=OFF ^
  -DKAGURA_PCH=OFF ^
  -DKAGURA_UNITY_BUILD=OFF
if %errorlevel% neq 0 ( echo [!] cmake config failed & exit /b 1 )

cmake --build build
if %errorlevel% neq 0 ( echo [!] build failed & exit /b 1 )

echo.
echo [+] build OK
dir /b build\lib\Transforms\*.dll