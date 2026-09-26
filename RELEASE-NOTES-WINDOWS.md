# Windows x86_64 发布说明

v0.2.1 修复周期 UDP 保活间隔错误：恢复原 Rust 客户端的 12 秒默认值，并允许两轮周期保活完全超时后再重连，避免单次 UDP 丢包造成短暂断网。

Windows 10/11 x64 版包含原生 GUI 和认证核心：

- GUI 读取物理以太网卡的实际 IPv4 与 MAC，编辑私有认证配置并显示实时日志；
- Npcap 发送和接收 EAPOL，Winsock 处理 DNS 与 DrCOM UDP；
- DNS/UDP socket 绑定选定物理网卡，避免修改默认路由，可与 Clash 共存；
- 支持独立的网卡实际 IPv4、EAP 认证 IPv4、UDP 载荷 IPv4和远端 UDP 服务器；
- 正常停止时发送 Logoff；
- EXE 静态链接 C++ 运行库和 OpenSSL。

运行前需安装 Npcap。私有配置默认保存在 `%APPDATA%\drcom4scut\config.conf`，不会包含在发布 ZIP 中。

发布 ZIP 含预编译 EXE 和 `install-windows.cmd`。安装脚本会检测现有 Npcap，检测通过时直接跳过，不会重复安装。

完整步骤见 `README-WINDOWS.md` 或仓库的 `docs/WINDOWS.md`。
