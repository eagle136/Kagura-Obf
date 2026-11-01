# Prebuilt binaries / 预编译产物

**English** | [简体中文](#简体中文)

## English

These plugins were built locally from this source tree and are published with the repository so they can be used without an LLVM development environment.

| Path | Platform | LLVM version | Notes |
|:-----|:---------|:-------------|:------|
| `linux-x86_64-llvm17.0.6/KaguraObfuscator.so` | Linux x86_64 | 17.0.6 (`llvm-project-17.0.6.src`) | Load with `clang -fpass-plugin=` / `opt --load-pass-plugin=` |
| `windows-x86_64-llvm17.0.6/KaguraObfuscator.dll` | Windows x86_64 | 17.0.6 | Exports `llvmGetPassPluginInfo` |
| `windows-x86_64-llvm19.1.7/KaguraObfuscator.dll` | Windows x86_64 | 19.1.7 | Exports `llvmGetPassPluginInfo` |
| `runtime/kagura_runtime.lib` | Windows x86_64 | LLVM-independent | Static C runtime |
| `include/kagura/*.h` | All | — | Public headers |

A pass plugin must be loaded by a `clang` / `opt` of the same LLVM major version. Choose the directory that matches your toolchain.

### Usage

```bash
clang -fpass-plugin=release/linux-x86_64-llvm17.0.6/KaguraObfuscator.so \
      -mllvm -kagura-str \
      -mllvm -kagura-fla \
      -mllvm -kagura-bcf \
      -O1 input.c -o input
```

```bat
clang -fpass-plugin=release\windows-x86_64-llvm17.0.6\KaguraObfuscator.dll ^
      -mllvm -kagura-str -mllvm -kagura-fla ^
      -O1 input.c -o input.exe
```

### Verify

```bash
sha256sum -c SHA256SUMS.txt
```

---

## 简体中文

这些插件由本仓库源码在本地编译，随仓库一起发布，免去自行准备 LLVM 开发环境。

| 路径 | 平台 | LLVM 版本 | 说明 |
|:-----|:-----|:----------|:-----|
| `linux-x86_64-llvm17.0.6/KaguraObfuscator.so` | Linux x86_64 | 17.0.6（`llvm-project-17.0.6.src`） | 通过 `clang -fpass-plugin=` 或 `opt --load-pass-plugin=` 加载 |
| `windows-x86_64-llvm17.0.6/KaguraObfuscator.dll` | Windows x86_64 | 17.0.6 | 导出 `llvmGetPassPluginInfo` |
| `windows-x86_64-llvm19.1.7/KaguraObfuscator.dll` | Windows x86_64 | 19.1.7 | 导出 `llvmGetPassPluginInfo` |
| `runtime/kagura_runtime.lib` | Windows x86_64 | 与 LLVM 版本无关 | 静态 C 运行时 |
| `include/kagura/*.h` | 全平台 | — | 公共头文件 |

Pass 插件必须由同一 LLVM 大版本的 `clang` / `opt` 加载，请选择与工具链匹配的目录。

### 使用

```bash
clang -fpass-plugin=release/linux-x86_64-llvm17.0.6/KaguraObfuscator.so \
      -mllvm -kagura-str \
      -mllvm -kagura-fla \
      -mllvm -kagura-bcf \
      -O1 input.c -o input
```

```bat
clang -fpass-plugin=release\windows-x86_64-llvm17.0.6\KaguraObfuscator.dll ^
      -mllvm -kagura-str -mllvm -kagura-fla ^
      -O1 input.c -o input.exe
```

### 校验

```bash
sha256sum -c SHA256SUMS.txt
```
