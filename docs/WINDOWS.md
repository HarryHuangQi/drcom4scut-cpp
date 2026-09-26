# Windows 10/11 GUI、Clash 共存与热点教程

## 1. 组成和工作方式

Windows 版包含两个程序：

- `drcom4scut-gui.exe`：原生 GUI，编辑配置、启动或停止认证并显示实时日志。
- `drcom4scut.exe`：认证核心。Npcap 负责 EAPOL，Winsock 负责校园 DNS 和 DrCOM UDP。

发布 ZIP 已经包含编译完成的两个 EXE，普通使用不需要安装编译器，也不需要自行编译源码。

程序不会修改系统 IPv4、默认路由、DNS、Clash、防火墙或移动热点。GUI 中的 `ip=` 和 `udp-ip=` 只写入认证报文。

认证核心把自己的 DNS 和 UDP socket 绑定到选定物理有线网卡的实际 IPv4，并设置 Windows `IP_UNICAST_IF`，因此普通应用可以继续通过 Clash 工作。

## 2. 安装要求

1. Windows 10/11 x64。
2. 物理以太网接口直接连接校园网端口。
3. Npcap。安装脚本会先检测，系统已经安装时不会重复安装。官方说明见 [Npcap Users' Guide](https://npcap.com/guide/npcap-users-guide.html)。
4. 保持 `drcom4scut.exe` 与 `drcom4scut-gui.exe` 在同一文件夹。

解压发布 ZIP 后双击 `install-windows.cmd`，或者在 PowerShell 运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\install-windows.ps1
```

安装脚本检查 `npcap` 服务以及 `System32\Npcap\wpcap.dll`、`Packet.dll`：

- 都存在：显示 `Npcap is already installed`，跳过 Npcap，直接安装两个现成 EXE；
- 不存在：如果 ZIP 目录旁有 `npcap-*.exe`，只在此时启动该安装程序；
- 不存在且没有本地安装程序：打开 [Npcap 官方下载页](https://npcap.com/#download)，安装后重新运行脚本。

Npcap 的公开免费安装包不随本项目重新分发。下载后也可以显式指定：

```powershell
.\install-windows.ps1 -NpcapInstaller "$HOME\Downloads\npcap-安装包文件名.exe"
```

程序默认安装到 `%LOCALAPPDATA%\Programs\drcom4scut`，并创建开始菜单快捷方式。

安装 Npcap 时保留默认设置即可。如果安装器提供 **WinPcap API-compatible Mode**，可以启用；程序会优先从 `C:\Windows\System32\Npcap` 加载 Npcap。

GUI 带有管理员权限清单，启动时出现 UAC 提示属于正常现象。管理员权限用于访问 Npcap 设备。

## 3. 设置网卡实际 IPv4

实际 IPv4属于 Windows 网卡设置，不属于认证配置文件。

图形操作：

1. 按 `Win + R`，输入 `ncpa.cpl`。
2. 找到接入校园网的“以太网”接口。
3. 右键 **属性**。
4. 打开 **Internet 协议版本 4 (TCP/IPv4)**。
5. 按学校或端口信息填写实际 IPv4、掩码、实际网关和 DNS。

管理员 PowerShell 查看命令：

```powershell
Get-NetAdapter
Get-NetIPAddress -AddressFamily IPv4
Get-NetRoute -AddressFamily IPv4
```

手动设置示例。以下是文档保留地址，必须替换：

```powershell
$IfName = '以太网'
$ActualIP = '192.0.2.10'
$PrefixLength = 24
$Gateway = '192.0.2.1'
$Dns = @('203.0.113.53', '203.0.113.54')

Set-NetIPInterface -InterfaceAlias $IfName -Dhcp Disabled
New-NetIPAddress `
  -InterfaceAlias $IfName `
  -IPAddress $ActualIP `
  -PrefixLength $PrefixLength `
  -DefaultGateway $Gateway

Set-DnsClientServerAddress `
  -InterfaceAlias $IfName `
  -ServerAddresses $Dns
```

修改正在用于远程桌面的网卡会断开连接，应在本地控制台操作。检查结果：

```powershell
Get-NetIPConfiguration -InterfaceAlias '以太网'
```

## 4. 使用 GUI 配置认证

双击 `drcom4scut-gui.exe`，依次填写：

| GUI 字段 | 配置项 | 说明 |
|---|---|---|
| 物理有线网卡 | `interface=` | 选择实际插网线的接口 |
| 网卡实际 IPv4 | 自动读取 | 来自 Windows TCP/IPv4 设置，只读 |
| 网卡实际 MAC | 自动读取 | 来自物理接口，只读 |
| EAP 认证 IPv4 | `ip=` | 写入 EAP Identity/MD5，可以不同于实际 IPv4 |
| UDP 载荷 IPv4 | `udp-ip=` | 写入 MiscInfo/Heartbeat3 |
| 实际有线网关 | `gateway=` | 当前物理网络的真实网关 |
| 认证 MAC | `mac=` | 默认使用选中接口的真实 MAC |
| UDP 服务器 | `host=` | 域名或抓包确认的服务器 IPv4 |
| 校园 DNS | `dns=` | 使用逗号分隔至少两个 DNS |

点击 **保存配置** 后，私有配置写入：

```text
%APPDATA%\drcom4scut\config.conf
```

该文件包含账号和密码，不要提交到 GitHub，也不要连同日志或抓包公开。

点击 **开始认证**。成功日志应包含：

```text
EAP authenticated
UDP session established
UDP startup heartbeat complete
```

点击 **停止认证** 时，GUI会通知核心正常退出，核心会尝试发送 Logoff。

## 5. 三类地址示例

假设：

```text
Windows 网卡实际地址：192.0.2.10/24
实际网关：192.0.2.1
EAP/UDP 认证地址：198.51.100.20
UDP 服务器：203.0.113.30:61440
```

Windows TCP/IPv4 中配置 `192.0.2.10/24` 和网关 `192.0.2.1`。GUI/配置文件中填写：

```ini
interface=以太网
profile=windows-31
ip=198.51.100.20
udp-ip=198.51.100.20
gateway=192.0.2.1
host=203.0.113.30
udp-local-port=61440
```

日志会显示：

```text
LocalIPv4=192.0.2.10 AuthIPv4=198.51.100.20 UdpIPv4=198.51.100.20
```

`203.0.113.30` 是远端服务器，不应配置到本机网卡。

## 6. 与 Clash 共存

客户端自身采取以下隔离措施：

- EAPOL 通过指定物理接口的 Npcap 设备发送，不经过 IP 代理。
- DNS 和 DrCOM UDP socket 绑定物理接口实际 IPv4。
- 两类 socket 都设置 `IP_UNICAST_IF` 指向物理接口。
- 程序不修改 Clash 或其他应用的路由。

把以下规则放在 Clash 最终 `MATCH`/兜底规则之前，并替换地址：

```yaml
rules:
  - IP-CIDR,UDP服务器IPv4/32,DIRECT,no-resolve
  - IP-CIDR,校园DNS1/32,DIRECT,no-resolve
  - IP-CIDR,校园DNS2/32,DIRECT,no-resolve
  # 其他规则……
  - MATCH,代理策略组
```

推荐在 `host=` 直接填写确认过的 UDP 服务器 IPv4，避免代理 DNS、Fake-IP 或认证前 DNS 不可用产生影响。

多数环境只需接口绑定和 `DIRECT` 规则。若 Clash 的严格 TUN/WFP 模式仍拦截，可增加精确 `/32` 主机路由；它们只影响认证服务器和校园 DNS：

```powershell
$IfIndex = 12                 # Get-NetAdapter 查到的物理接口索引
$Gateway = '192.0.2.1'       # 实际有线网关
$UdpServer = '203.0.113.30'  # 实际 UDP 服务器
$Dns1 = '203.0.113.53'
$Dns2 = '203.0.113.54'

route -p add $UdpServer mask 255.255.255.255 $Gateway if $IfIndex metric 1
route -p add $Dns1 mask 255.255.255.255 $Gateway if $IfIndex metric 1
route -p add $Dns2 mask 255.255.255.255 $Gateway if $IfIndex metric 1
```

删除可选路由：

```powershell
route delete $UdpServer
route delete $Dns1
route delete $Dns2
```

不要把 Clash、Wintun 或热点虚拟接口选为认证接口。无需关闭 Clash，也不要修改系统默认路由。

## 7. Windows 移动热点和转发

Windows 移动热点由系统提供 DHCP、DNS 和 NAT，无需本程序修改转发规则。

1. 先完成认证，确认 Windows 本机通过物理以太网可以访问公网。
2. 打开 **设置 → 网络和 Internet → 移动热点**。
3. “共享我的 Internet 连接来源”选择已认证的物理 **以太网**。
4. “通过以下方式共享”选择 **Wi-Fi**。
5. 编辑热点名称和强密码，然后开启移动热点。

微软说明：[Use your Windows device as a mobile hotspot](https://support.microsoft.com/windows/c89b0fad-72d5-41e8-f7ea-406ad9036b85)。

为了不干扰 Clash 和主机正常工作：

- 上游选择物理以太网，不选 Clash/Wintun 虚拟适配器。
- 不创建网络桥接。
- 不手工替换默认路由。
- 认证核心保持绑定物理以太网；热点客户端由 Windows ICS 转发。
- 若希望热点客户端也使用 Clash，应在 Clash 中单独启用局域网访问或 TUN 转发。

热点连接成功但没有公网时：

1. 确认主机经物理以太网能上网。
2. 确认移动热点共享来源是物理以太网。
3. 检查 `SharedAccess`（Internet Connection Sharing）服务。
4. 关闭再开启移动热点，让 Windows 重建 ICS/NAT。

## 8. 命令行模式

```powershell
# 列出接口
.\drcom4scut.exe --list-interfaces

# 只检查接口/Npcap，不发送认证报文；中文名称有编码问题时使用数字索引
.\drcom4scut.exe --interface 12 --check-interface

# 使用 GUI 保存的配置运行一次
.\drcom4scut.exe `
  --config "$env:APPDATA\drcom4scut\config.conf" `
  --once
```

## 9. 构建 Windows EXE

安装 MSYS2 UCRT64 工具链和 OpenSSL 后运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\windows\build-windows.ps1
```

生成：

```text
bin\drcom4scut.exe
bin\drcom4scut-gui.exe
```

发布 EXE 静态链接 C++ 运行库和 OpenSSL，仅动态依赖 Windows 系统 DLL；Npcap 由系统安装并在运行时加载。
