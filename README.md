# drcom4scut-cpp

面向 Linux 的 DrCOM/802.1X 校园网认证客户端，使用 C++17、Linux `AF_PACKET`、UDP 和 OpenSSL 实现。

该版本根据原 Rust 工程移植，并增加了当前 Windows 客户端抓包中观察到的 `windows-31` 流程：260 字节 MiscInfo、独立 EAP/UDP 认证 IPv4、网关和双 DNS 字段、UDP 启动心跳以及可配置的常规心跳周期。
可以让国际校区本科生宿舍使用研究生有线端访问互联网
建议让llm辅助阅读和配置

## 支持平台

| 平台 | 安装入口 | 状态 |
|---|---|---|
| Ubuntu 22.04 x86_64 | `install-ubuntu22.04-x86_64.sh` | 已在 Ubuntu 22.04.5 用户空间完成编译和集成测试 |
| Ubuntu 18.04/20.04/22.04 ARM64 | `install-ubuntu22.04-arm64.sh` | 从源码原生编译 |
| Jetson Linux / Jetson Nano | `install-jetson-nano.sh` | 从源码原生编译并且成功实际使用 |

程序只能在 Linux 上运行。实际认证需要直接连接校园网的物理以太网卡；WSL2 适合编译和测试，通常不能将 EAPOL 二层报文直接发送到 Windows 主机的物理网口。
windows版还在开发中

## 下载与安装

从 GitHub Release 下载与架构对应的压缩包和 `SHA256SUMS`：
验证哈希值可以省略，是傻鸟gpt的神秘小巧思，纯浪费我token

```bash
sha256sum -c SHA256SUMS --ignore-missing
tar -xzf drcom4scut-cpp-v0.1.0-ubuntu22.04-x86_64.tar.gz
cd drcom4scut-cpp-v0.1.0
sudo bash install-ubuntu22.04-x86_64.sh
```

ARM64 使用：

```bash
tar -xzf drcom4scut-cpp-v0.1.0-ubuntu22.04-arm64.tar.gz
cd drcom4scut-cpp-v0.1.0
sudo bash install-ubuntu22.04-arm64.sh
```

Jetson 使用：

```bash
sudo bash install-jetson-nano.sh
```

安装程序会安装构建依赖、编译 Release 版本、运行协议测试，然后安装：

```text
/usr/local/bin/drcom4scut
/etc/drcom4scut/config.conf
/etc/systemd/system/drcom4scut.service
```

它不会自动启用服务或发送认证报文。

完整配置过程见 [安装说明](docs/INSTALL.md)。

认证成功后需要把有线网络分享为 Wi-Fi 热点时，参见 [网卡与热点转发](docs/HOTSPOT.md)。

## 配置要点

### 实际 IPv4、认证 IPv4与 UDP 服务器分别配置在哪里

这几项属于不同层次，不能相互替代：

| 参数 | 配置位置 | 含义 |
|---|---|---|
| 有线接口实际 IPv4/前缀 | NetworkManager、Netplan 或系统网络配置 | Linux 真正绑定在物理网卡上的地址，也是 UDP socket 实际源地址 |
| 实际网关 | 系统网络配置，同时填写 `config.conf` 的 `gateway=` | 系统用它路由；`windows-31` 也会把它写入 MiscInfo |
| EAP 认证地址 | `/etc/drcom4scut/config.conf` 的 `ip=` | 写入 EAP Identity 和 EAP-MD5，不会自动添加到 Linux 网卡 |
| UDP 载荷地址 | `config.conf` 的 `udp-ip=` | 写入 DrCOM MiscInfo 和 Heartbeat3；可以与实际 IPv4不同 |
| UDP 服务器 | `config.conf` 的 `host=` | DrCOM UDP 服务端的域名或 IPv4，不是本机地址 |
| UDP 本地端口 | `config.conf` 的 `udp-local-port=` | 客户端绑定的本地 UDP 端口；当前协议通常使用 `61440` |
| 认证 MAC | `config.conf` 的 `mac=` | 省略时自动读取 `interface=` 对应物理网卡的真实 MAC |

例如，假设学校或端口实际分配的信息是：

```text
物理接口：enp3s0
实际 IPv4：192.0.2.10/24
实际网关：192.0.2.1
账户登记认证地址：198.51.100.20
抓包确认的 UDP 服务器：203.0.113.30:61440
```
udp在和地址和udp服务器地址不同，udp载荷地址目前使用时是和认证地址一致却和udp服务器地址不同，但是为了未来可能出现的不同情况还是保留该设置



以上三个网段是文档示例地址，使用时必须替换成自己的数据。首先把实际地址配置到 Linux 网卡：

```bash
sudo nmcli connection add \
  type ethernet \
  ifname enp3s0 \
  con-name campus-port \
  ipv4.method manual \
  ipv4.addresses 192.0.2.10/24 \
  ipv4.gateway 192.0.2.1 \
  ipv4.dns "学校DNS1 学校DNS2" \
  ipv6.method disabled \
  connection.autoconnect yes

sudo nmcli connection up campus-port
```

随后在认证配置中填写：

```ini
interface=enp3s0
profile=windows-31

ip=198.51.100.20
udp-ip=198.51.100.20

gateway=192.0.2.1

username=校园网账号
password=校园网密码

host=203.0.113.30
udp-local-port=61440
```

运行时日志会把三类地址分开打印：

```text
LocalIPv4=192.0.2.10 AuthIPv4=198.51.100.20 UdpIPv4=198.51.100.20
```

- `LocalIPv4` 来自 Linux 网卡配置，程序只读取，不修改。
- `AuthIPv4` 来自 `ip=`。
- `UdpIPv4` 来自 `udp-ip=`；未配置时才默认使用 `LocalIPv4`。
- UDP 服务器来自 `host=`，成功 EAP 后日志会显示实际连接的服务端地址。

如果账户登记地址与网卡实际地址相同，可以让 `ip=` 使用实际地址，并省略 `udp-ip=`。如果两者不同，应以成功官方客户端的抓包和服务器要求为准；不要把只用于认证载荷的地址额外绑定到 Linux 网卡。

复制并编辑示例：

```bash
sudo install -d -m 0755 /etc/drcom4scut
sudo install -o root -g root -m 0600 config.example /etc/drcom4scut/config.conf
sudo nano /etc/drcom4scut/config.conf
```

配置使用 `key=value`，不是原 Rust 客户端的 YAML。至少需要填写：

```ini
interface=enp3s0
profile=windows-31
ip=账户登记的认证IPv4
udp-ip=UDP载荷使用的IPv4
gateway=物理有线网络的网关
username=校园网账号
password=校园网密码
```

`interface` 对应的 Linux 网卡必须已经拥有实际 IPv4。`ip` 和 `udp-ip` 是写入认证报文的地址，可以与网卡实际地址不同。程序不会执行 DHCP，也不会修改系统地址、路由或 DNS。

MAC 默认从物理接口读取；只有协议明确要求其他值时才配置 `mac=`。
注意有线网实际ipv4和网关以及子网掩码，应该由网络中心提供
udp服务器不同校区可能不一样

## 前台测试

```bash
sudo systemctl stop drcom4scut.service
sudo drcom4scut --config /etc/drcom4scut/config.conf --once
```

成功流程输出通常包含：

```text
EAP authenticated
UDP session established
UDP startup heartbeat complete
```

确认成功后启用后台服务：

```bash
sudo systemctl enable --now drcom4scut.service
sudo journalctl -u drcom4scut.service -f
```

## 网卡与热点转发快速示例

先确认有线认证成功，并让默认路由经过有线接口：

```bash
WIRED_IF=enp3s0
WIFI_IF=wlp4s0
LOCAL_IP='实际有线IPv4'
GATEWAY='实际有线网关'

sudo ip route replace default \
  via "$GATEWAY" \
  dev "$WIRED_IF" \
  src "$LOCAL_IP" \
  metric 50
```

确认无线网卡支持 AP：

```bash
iw list | sed -n '/Supported interface modes:/,/Band/p'
```

列表中存在 `AP` 后创建热点：

```bash
sudo nmcli connection add \
  type wifi \
  ifname "$WIFI_IF" \
  con-name campus-hotspot \
  ssid Campus-Hotspot

sudo nmcli connection modify campus-hotspot \
  802-11-wireless.mode ap \
  802-11-wireless.band bg \
  wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk '请换成强热点密码' \
  ipv4.method shared \
  ipv4.addresses 10.42.0.1/24 \
  ipv6.method disabled \
  connection.autoconnect yes

sudo sysctl -w net.ipv4.ip_forward=1
echo 'net.ipv4.ip_forward=1' | \
  sudo tee /etc/sysctl.d/90-campus-hotspot.conf

sudo nmcli connection up campus-hotspot
```

`ipv4.method shared` 会让 NetworkManager 提供 DHCP、DNS 转发和 NAT。热点客户端应获得 `10.42.0.x`，网关为 `10.42.0.1`，公网出口为物理有线接口。完整的远程 SSH 切换、检查、备用防火墙规则和恢复步骤见 [docs/HOTSPOT.md](docs/HOTSPOT.md)。

## 从源码构建

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev python3 iproute2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

需要 root 权限的 Linux 网络集成测试：

```bash
sudo python3 tests/linux_integration.py ./build/drcom4scut
```

## 验证状态

最终源码已在 Ubuntu 22.04 arm64 环境通过：

- GCC 11.4、CMake 3.22、OpenSSL 3.0 编译；
- Linux network namespace/veth 集成测试；
- EAPOL、EAP-MD5、DNS、UDP 会话、启动心跳、常规心跳、重试、超时和 SIGTERM 路径。
- 实际使用测试

测试使用模拟认证端，不包含真实账号。真实服务器兼容性取决于校区、交换机端口和当前服务端协议。

## 凭据安全

不要提交真实 `config.conf`、抓包文件或日志。公开问题前应删除账号、密码、MAC、认证 IPv4、端口 IPv4及可能包含这些数据的报文十六进制内容。

## 来源与许可证

协议实现来自 SeaLoong/drcom4scut Rust 工程的移植和抓包兼容更新。详情见 [NOTICE.md](NOTICE.md)。许可证文本见 [LICENSE](LICENSE)。
