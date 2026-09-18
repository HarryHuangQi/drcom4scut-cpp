# v0.1.0 发布说明

首个 Linux C++ 发布版本，支持 Ubuntu x86_64、Ubuntu ARM64 和 Jetson Linux。

## 下载选择

- `drcom4scut-cpp-v0.1.0-source.tar.gz`：纯源码。
- `drcom4scut-cpp-v0.1.0-ubuntu22.04-x86_64.tar.gz`：源码、安装脚本及 Ubuntu 22.04 x86_64 预编译 ELF。
- `drcom4scut-cpp-v0.1.0-ubuntu22.04-arm64.tar.gz`：源码和 ARM64/Jetson 原生编译安装脚本。
- `SHA256SUMS`：全部发布包的 SHA-256。

## 已验证内容

- Ubuntu 22.04 x86_64 原生工具链编译。

- Linux `AF_PACKET`、DNS、EAP-MD5、UDP 建链、启动心跳、常规心跳、重试和退出流程。

首次使用前必须编辑 `/etc/drcom4scut/config.conf`。发布包不包含任何真实账号、密码、MAC 或个人 IP 配置。
