# PP-OCRv6 / ncnn Vulkan sidecar

This directory contains the C++ PP-OCRv6 detector / classifier / recognizer
implementation used by `runtime/ocr-ncnn`.

The OCR engine is based on the MIT-licensed
[Avafly/PaddleOCR-ncnn-CPP](https://github.com/Avafly/PaddleOCR-ncnn-CPP)
project and is built against ncnn with `NCNN_VULKAN=ON` and OpenCV 4.x.

The sidecar uses the same stdin/stdout protocol as the legacy `ocr-fast`
sidecar:

```text
READY
GPU|CPU
<image path>
OK <byte-count>
<utf-8 text>
```

Build example:

```bat
cmake -S cpp\ocr-ncnn -B build\ocr-ncnn ^
  -G "Visual Studio 18 2026" -A x64 ^
  -DOpenCV_DIR=<opencv>\build\x64\vc16\lib ^
  -Dncnn_DIR=<ncnn>\x64\lib\cmake\ncnn
cmake --build build\ocr-ncnn --config Release
```

`runtime/ocr-ncnn` is the deployable binary runtime: copy `ocr_ncnn.exe`,
`ncnn.dll`, `opencv_world4110.dll`, the VC runtime DLLs, `config-*.json`, and
`models/` next to each other.
