# 第三方组件

本项目在源码树中直接内置了以下第三方组件（`third_party/`），构建时静态链接或嵌入资源。各组件版权归原作者所有，遵循其自身许可证；本项目对其进行的使用不改变其许可证。

| 组件 | 版本 | 位置 | 许可证 |
|---|---|---|---|
| cJSON | 1.7.x | `third_party/cjson` | MIT |
| libssh2 | 1.11.1 | `third_party/libssh2-1.11.1` | BSD-3-Clause |
| libsmb2 | 6.3 | `third_party/libsmb2-libsmb2-6.3` | LGPL-2.1 |
| Microsoft WebView2 SDK | 1.0.x | `third_party/webview2` | Microsoft 软件许可条款 |
| xterm.js | 5.5.0 | web/vendor/xterm | MIT |
| LibreSpeed | 前端资源 | `web/speedtest` | LGPL-3.0 |

## 说明

- **libssh2** 提供 SSH 客户端能力（仅 Windows 构建启用，WinCNG 后端）。
- **libsmb2** 提供 SMB2/3 客户端能力，以静态库形式链接。
- **WebView2 SDK** 仅使用其头文件与 `WebView2Loader.dll`（运行时由系统 WebView2 Runtime 提供）。
- **xterm.js** 提供终端仿真（ANSI 颜色、光标、滚轮回看），以静态资源形式嵌入，MIT 许可。
- **LibreSpeed** 前端页面随程序分发，未修改其许可证声明；测速后端为本项目自行实现。

各组件的完整许可证文本位于其各自目录下（如 `third_party/libssh2-1.11.1/COPYING`）。若需重新分发本项目的二进制产物，请一并遵守上述许可证的署名与源码提供要求。
