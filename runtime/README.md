# Runtime dependencies

- `weijie-ocr-ncnn.zip`: PP-OCRv6 small + ncnn Vulkan 组件、DLL 和模型。
  设置页的「下载 OCR 组件和 PP 模型」按钮从
  `https://raw.githubusercontent.com/CangShui/weijie/main/runtime/weijie-ocr-ncnn.zip`
  下载并解压到 `%LOCALAPPDATA%\weijie\ocr-ncnn`。
- `ocr-ncnn/`: 开发机构建/测试时从上面的 ZIP 解压出的工作副本，不提交到仓库。
- `images/`: 默认图片与表情素材，随基础包分发。
  `images/default` 显示为“默认”分组；用户可自行增删图片或创建子文件夹分组。

用户设置仍在 `%APPDATA%\weijie`，下载组件和日志在 `%LOCALAPPDATA%\weijie`。
