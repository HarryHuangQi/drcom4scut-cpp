# 安装与配置

## 1. 选择发布包

- x86_64 Ubuntu：`drcom4scut-cpp-v0.2.2-ubuntu22.04-x86_64.tar.gz`
- ARM64 Ubuntu/Jetson：`drcom4scut-cpp-v0.2.2-ubuntu22.04-arm64.tar.gz`
- 纯源码：`drcom4scut-cpp-v0.2.2-source.tar.gz`

校验下载文件：

```bash
sha256sum -c SHA256SUMS --ignore-missing
```

如果校验文件经过 Windows 编辑并带有 CRLF：

```bash
tr -d '\r' < SHA256SUMS | sha256sum -c --ignore-missing
```

## 2. 安装

x86_64：

```bash
tar -xzf drcom4scut-cpp-v0.2.2-ubuntu22.04-x86_64.tar.gz
cd drcom4scut-cpp-v0.2.2
sudo bash install-ubuntu22.04-x86_64.sh
```

ARM64：

```bash
tar -xzf drcom4scut-cpp-v0.2.2-ubuntu22.04-arm64.tar.gz
cd drcom4scut-cpp-v0.2.2
sudo bash install-ubuntu22.04-arm64.sh
```

Jetson Nano/Jetson Linux：

```bash
sudo bash install-jetson-nano.sh
```

通用安装入口是：

```bash
sudo bash install-ubuntu22.04.sh
```

安装脚本支持 Ubuntu 18.04、20.04、22.04 的 x86_64 和 AArch64。脚本完成后不会启动服务。

## 3. 找到物理有线接口

```bash
ip -brief link
drcom4scut --list-interfaces
nmcli device status
```

常见接口名包括 `eth0`、`eno1`、`enp2s0`。下文以 `enp3s0` 为例：

```bash
WIRED_IF=enp3s0
ip -brief link show dev "$WIRED_IF"
cat "/sys/class/net/$WIRED_IF/address"
```

选择直接连接校园网墙口或交换机的物理以太网接口。

## 4. 先配置网卡实际 IPv4

认证程序要求接口已经拥有实际 IPv4。可以使用 DHCP，也可以按学校提供的地址、前缀、网关和 DNS 手动配置。

DHCP 示例：

```bash
sudo nmcli connection add \
  type ethernet \
  ifname "$WIRED_IF" \
  con-name campus-port \
  ipv4.method auto \
  ipv6.method disabled \
  connection.autoconnect yes
sudo nmcli connection up campus-port
```

手动 IPv4 示例。先替换变量，不能原样使用占位文字：

```bash
LOCAL_CIDR='实际端口IPv4/前缀长度'
GATEWAY='实际网关IPv4'
DNS_SERVERS='学校DNS1 学校DNS2'

sudo nmcli connection add \
  type ethernet \
  ifname "$WIRED_IF" \
  con-name campus-port \
  ipv4.method manual \
  ipv4.addresses "$LOCAL_CIDR" \
  ipv4.gateway "$GATEWAY" \
  ipv4.dns "$DNS_SERVERS" \
  ipv4.route-metric 1000 \
  ipv6.method disabled \
  connection.autoconnect yes

sudo nmcli connection up campus-port
```

检查：

```bash
ip -brief -4 address show dev "$WIRED_IF"
ip route
ip neigh show dev "$WIRED_IF"
```

网卡实际 IPv4 和认证报文 IPv4 是两个独立概念。不要因为账号登记地址不同，就把没有分配给当前端口的地址直接配置到 Linux 网卡上。

## 5. 创建认证配置

```bash
sudo install -d -m 0755 /etc/drcom4scut
sudo install -o root -g root -m 0600 \
  config.example /etc/drcom4scut/config.conf
sudo nano /etc/drcom4scut/config.conf
```

最小 `windows-31` 配置：

```ini
interface=enp3s0
profile=windows-31
ip=账户登记的认证IPv4
udp-ip=UDP载荷使用的IPv4
gateway=实际有线网关
username=校园网账号
password=校园网密码
host=s.scut.edu.cn
udp-local-port=61440
dns=学校DNS1,学校DNS2
udp-trailer=2001025030007004aa0cb7dee93f3c65
heartbeat-interval=12
```

MAC 默认自动读取。需要明确指定时加入：

```ini
mac=00:11:22:33:44:55
```

保护配置文件：

```bash
sudo chown root:root /etc/drcom4scut/config.conf
sudo chmod 600 /etc/drcom4scut/config.conf
sudo grep -Ev '^(password)=' /etc/drcom4scut/config.conf
```

## 6. 前台测试

```bash
sudo systemctl stop drcom4scut.service
sudo pkill -x drcom4scut 2>/dev/null || true
sudo drcom4scut \
  --config /etc/drcom4scut/config.conf \
  --once 2>&1 | tee "$HOME/drcom-test.log"
```

成功日志通常按以下顺序出现：

```text
EAP Start
EAP Identity response
EAP MD5 response
EAP authenticated
UDP MiscInfo sent
UDP session established
UDP startup heartbeat sent
UDP startup heartbeat complete
```

日志开头会打印：

```text
LocalIPv4=接口实际地址 AuthIPv4=认证地址 UdpIPv4=UDP载荷地址
```

三个地址不必相同。

## 7. systemd 后台服务

```bash
sudo systemctl enable --now drcom4scut.service
sudo systemctl is-active drcom4scut.service
sudo journalctl -u drcom4scut.service -f
```

修改配置后：

```bash
sudo systemctl restart drcom4scut.service
```

停止并禁止自启动：

```bash
sudo systemctl disable --now drcom4scut.service
```

## 8. 测试公网

指定有线接口，避免测试请求从 Wi-Fi 发出：

```bash
curl --interface "$WIRED_IF" \
  --ipv4 \
  --connect-timeout 5 \
  --max-time 15 \
  -I https://www.baidu.com/
```

查看实际源地址：

```bash
curl --interface "$WIRED_IF" \
  --ipv4 \
  --silent \
  --show-error \
  --output /dev/null \
  --write-out 'local=%{local_ip} remote=%{remote_ip} http=%{http_code}\n' \
  https://www.baidu.com/
```

单个测试站点可能被网络策略拒绝，应使用多个正常网站交叉验证。

## 9. 常见错误

### `interface needs an IPv4 address`

接口没有实际 IPv4。检查网线、接口名和 NetworkManager 连接：

```bash
nmcli device status
ip -brief -4 address show dev "$WIRED_IF"
```

### EAP 服务器拒绝

检查账号、密码、认证 IP 和 MAC。不要用接口实际地址随意覆盖账号登记的认证地址。

### `cannot resolve server`

认证前 DNS 可能不可用。可以把 `host=` 改为从成功客户端日志或抓包确认的 UDP 服务器 IPv4。

### EAP 成功但 UDP 超时

检查 `profile=windows-31`、`udp-ip`、`gateway`、双 DNS、`udp-local-port` 和 `udp-trailer`。还应确认 UDP 服务器路由经过目标有线接口。

## 10. 抓包

```bash
sudo timeout 45 tcpdump \
  -i "$WIRED_IF" \
  -s0 \
  -nn \
  -w "$HOME/drcom-test.pcap" \
  'ether proto 0x888e or udp port 61440'
```

抓包、日志和配置可能包含个人网络信息，公开前必须脱敏。
