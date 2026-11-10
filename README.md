# 微截 · 源码仓库

原生 C++20 / Win32 / GDI+ 截图工具。基础产物不包含 OCR 组件；
用户在设置「文字识别」页点击下载后，才会把 PP-OCRv6 small + ncnn Vulkan
组件和模型安装到 `%LOCALAPPDATA%\weijie\ocr-ncnn`。

仓库：<https://github.com/CangShui/weijie>

## 目录

```text
assets/                     图标与源素材
cpp/src/                    当前原生 C++ 源码
cpp/tests/                  核心测试与原生 UI 快照测试
cpp/ocr-ncnn/               PP-OCRv6 ncnn sidecar 源码
runtime/weijie-ocr-ncnn.zip PP-OCRv6 small + ncnn Vulkan 组件与模型（设置页下载源）
runtime/ocr-ncnn/           开发机解压后的 OCR 组件（本地测试用，不入库）
runtime/emoji/              彩色 Emoji PNG
docs/                       发布说明、源项目文档
scripts/                    构建、运行、打包脚本
legacy/                     旧 Python 版源码（历史保留）
Release/                    构建产物目录
```

## 构建

```bat
build.bat -Task Build
build.bat -Task Test -Suite CPU
build.bat -Task Test -Suite GPU
build.bat -Task Test -Suite UI
```

`-Task Build` 生成最小运行包 `Release\weijie\微截.exe`，不复制 OCR 组件。
CPU / GPU 测试会自动从 `runtime\weijie-ocr-ncnn.zip` 解压组件到测试目录，
测试结束后再移除，保证最终发布目录仍然是最小产物。

输出结构：

```text
Release/weijie/             最小运行包：微截.exe、emoji、快捷方式
Release/Tests/              核心测试和 UI 测试程序
Release/verification/       测试日志、UI 快照
Release/.build/             编译中间文件，可安全清理
Release/archives/           可选 ZIP 包
```
