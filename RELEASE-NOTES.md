# v0.2.3 发布说明

本版在 Windows、Ubuntu x86_64 和 ARM64/Jetson 上统一调整在线保持策略：周期 UDP 保活回包连续超时不会主动注销健康的 EAP 会话，客户端会继续发送 UDP 保活，只有 EAP 心跳超时或服务器拒绝时才重新认证。Windows GUI 继续提供精简和完整配置模式。

## 下载选择

- `drcom4scut-cpp-v0.2.3-source.tar.gz`：完整 Linux/Windows 源码。
- `drcom4scut-cpp-v0.2.3-ubuntu22.04-x86_64.tar.gz`：源码、安装脚本及 Ubuntu 22.04 x86_64 预编译 ELF。
- `drcom4scut-cpp-v0.2.3-ubuntu22.04-arm64.tar.gz`：源码和 ARM64/Jetson 原生编译安装脚本。
- `drcom4scut-cpp-v0.2.3-windows-x86_64.zip`：Windows GUI 和认证核心。
- `SHA256SUMS`：全部发布包的 SHA-256。

## 已验证内容

- Ubuntu 22.04 x86_64 原生工具链编译。

- Linux `AF_PACKET`、DNS、EAP-MD5、UDP 建链、启动心跳、常规心跳、重试和退出流程。

首次使用前必须编辑 `/etc/drcom4scut/config.conf`。发布包不包含任何真实账号、密码、MAC 或个人 IP 配置。
