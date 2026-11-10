# 简截 v1.0.5 发布说明

> 这份内容与 GitHub Releases 页面上的正文一致。

## 光标缩小 20%

v1.0.4 换上的蓝色箭头整体偏大，压在画面上有点抢镜。这次按原尺寸缩到 **80%**
（33px → 26.4px，高 DPI 下同比放大），形状、渐变、描边一点没动。

其余行为不变：

- 箭头尖就是落点
- 拖控制点、拖选区时仍然给系统的缩放 / 移动光标
- 光标是真正的 `HICON`（`SetCursor`），不画在画布上，不会进截图结果

## 下载

| 文件 | 大小 | 说明 |
|---|---|---|
| [`JianJie-v1.0.5-win64.exe`](https://github.com/QingHua27/Simple-screenshot/releases/download/v1.0.5/JianJie-v1.0.5-win64.exe) | 439 KB | 单文件免安装，双击即用 |

- **系统要求**：Windows 10 / 11 64 位，不需要 .NET 或 VC 运行库
- 覆盖安装即可，配置 `%APPDATA%\JianJie\settings.json` 不受影响

## 校验

```
SHA256  0b23ef20d4bc9f79eb1f8f9e5ba0695d485f1aaa817caab993ce3ff59523ce61
大小    449536 字节
```
