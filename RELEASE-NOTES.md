# v0.2.1 发布说明

本版同步修复 Windows、Ubuntu x86_64、Ubuntu ARM64 和 Jetson Linux 的周期 UDP 保活。默认间隔恢复为原 Rust 客户端使用的 12 秒，并允许两轮周期保活完全超时后再重连，避免偶发丢包或 300 秒错误间隔造成短暂断网。

## 下载选择

- `drcom4scut-cpp-v0.2.1-source.tar.gz`：完整 Linux/Windows 源码。
- `drcom4scut-cpp-v0.2.1-ubuntu22.04-x86_64.tar.gz`：源码、安装脚本及 Ubuntu 22.04 x86_64 预编译 ELF。
- `drcom4scut-cpp-v0.2.1-ubuntu22.04-arm64.tar.gz`：源码和 ARM64/Jetson 原生编译安装脚本。
- `drcom4scut-cpp-v0.2.1-windows-x86_64.zip`：Windows GUI 和认证核心。
- `SHA256SUMS`：全部发布包的 SHA-256。

## 已验证内容

- Ubuntu 22.04 x86_64 原生工具链编译。

- Linux `AF_PACKET`、DNS、EAP-MD5、UDP 建链、启动心跳、常规心跳、重试和退出流程。

首次使用前必须编辑 `/etc/drcom4scut/config.conf`。发布包不包含任何真实账号、密码、MAC 或个人 IP 配置。
