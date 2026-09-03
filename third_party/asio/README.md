# standalone Asio (vendored)

- 上游: https://github.com/chriskohlhoff/asio
- 版本: asio-1-28-2 (release tag, non-Boost standalone Asio)
- 内容: 仅头文件 (header-only) + Boost Software License
- 用途: websocketpp 的底层网络事件库；`ASIO_STANDALONE` 模式使用
- 平台: Windows 下需链接 ws2_32

## 修改

无，与上游 asio-1-28-2 tag 的 include 目录完全一致。
