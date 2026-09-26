# Changelog

## 0.2.1

- Restored the original Rust client's 12-second UDP heartbeat interval.
- Tolerate two complete periodic heartbeat timeouts before reconnecting, while keeping startup authentication timeouts strict.

## 0.2.0

- Added a native Windows 10/11 x86_64 authentication core using Npcap and Winsock.
- Added a Win32 GUI for adapter selection, private configuration and live logs.
- Bound Windows DNS and DrCOM UDP sockets to the selected physical adapter for Clash coexistence.
- Added Windows build, release and mobile-hotspot documentation.

## v0.1.0

- 将核心认证流程移植为 Linux C++17 实现。
- 支持 x86_64 和 AArch64 Ubuntu。
- 增加 `windows-31`、260 字节 MiscInfo、网关、双 DNS 和 UDP 尾部。
- 区分接口实际 IPv4、EAP 认证 IPv4 和 UDP 载荷 IPv4。
- 增加当前协议的 UDP 启动心跳和可配置常规心跳间隔。
- 提供 systemd 服务、Ubuntu/Jetson 安装脚本和公开配置模板。
- 增加 297 项协议检查及 Linux namespace/veth 集成测试。
