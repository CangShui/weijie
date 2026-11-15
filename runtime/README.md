# Runtime dependencies

模型权重与运行时不再随仓库分发，统一下载到
<https://github.com/CangShui/weijie/releases/tag/models>：

| 资产 | 说明 | 安装位置 |
|---|---|---|
| `weijie-llama-runtime.zip` | llama.cpp Vulkan 运行时（识别与翻译共用） | `models\runtime\llama` |
| `weijie-ocr-ncnn.zip` | PP-OCRv6 small + ncnn Vulkan 组件 | `models\runtime\ppocr-ncnn` |
| `HunyuanOCR-Q8_0.gguf` | 混元 OCR 高精度权重 | `models\weights` |
| `HunyuanOCR-Q3_K_S.gguf` | 混元 OCR 轻量权重 | `models\weights` |
| `mmproj-HunyuanOCR-Q8_0.gguf` | 混元 OCR 视觉投影 | `models\weights` |
| `Haidass-Translate-143M.Q8_0.gguf` | 本地翻译，中英双向 | `models\weights` |
| `Qwen3.5-0.8B-Q8_0.gguf` | 本地翻译，通用小模型 | `models\weights` |
| `Index-Translate-2B.Q4_K_M.gguf` | 本地翻译，默认 | `models\weights` |
| `Index-Translate-2B.Q8_0.gguf` | 本地翻译，近无损 | `models\weights` |

设置页按需下载上述文件，支持断点续传、多线程和 SHA-256 校验。
`models` 目录位于主程序所在文件夹，升级或重新部署时保留，不会被安装脚本覆盖。

- `ocr-ncnn/`: 开发机构建/测试时解压出的工作副本，不提交到仓库。
- `images/`: 默认图片与表情素材，随基础包分发。
  `images/default` 显示为“默认”分组；用户可自行增删图片或创建子文件夹分组。

用户设置仍在 `%APPDATA%\weijie`，日志在 `%LOCALAPPDATA%\weijie`。
