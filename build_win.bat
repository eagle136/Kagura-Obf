@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d D:\tmp\kagura-main
if not exist build mkdir build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=D:\llvm-install\lib\cmake\llvm -DKAGURA_BUILD_TESTS=OFF -DKAGURA_BITCODE_TOOLS=OFF -DKAGURA_PCH=ON
if %errorlevel% neq 0 ( echo [!] cmake config failed & exit /b 1 )
cmake --build build
