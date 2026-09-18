# 网卡配置与 Wi-Fi 热点转发

本教程使用物理有线网卡作为校园网和公网入口，将无线网卡切换成 AP，为手机、电脑等客户端提供 DHCP、DNS 转发和 NAT。

```text
热点客户端 10.42.0.x
        │ Wi-Fi
        ▼
无线接口 10.42.0.1/24
        │ IPv4 forwarding + NAT
        ▼
有线接口 实际端口IPv4
        │ 实际网关
        ▼
校园网/公网
```

## 1. 设置接口变量

查找接口：

```bash
ip -brief link
nmcli device status
```

根据自己的电脑修改：

```bash
WIRED_IF=enp3s0
WIFI_IF=wlp4s0
```

Jetson 上通常是：

```bash
WIRED_IF=eth0
WIFI_IF=wlan0
```

不要把 NetworkManager 连接名称当作接口名称。`WIRED_IF` 和 `WIFI_IF` 必须是 `ip link` 显示的设备名。

## 2. 配置有线网卡

### DHCP

```bash
sudo nmcli connection delete campus-port 2>/dev/null || true
sudo nmcli connection add \
  type ethernet \
  ifname "$WIRED_IF" \
  con-name campus-port \
  ipv4.method auto \
  ipv4.route-metric 50 \
  ipv6.method disabled \
  connection.autoconnect yes
sudo nmcli connection up campus-port
```

### 手动 IPv4

先填写端口实际网络参数：

```bash
LOCAL_CIDR='实际IPv4/前缀长度'
LOCAL_IP='实际IPv4'
GATEWAY='实际网关IPv4'
DNS_SERVERS='学校DNS1 学校DNS2'
```

创建连接：

```bash
sudo nmcli connection delete campus-port 2>/dev/null || true
sudo nmcli connection add \
  type ethernet \
  ifname "$WIRED_IF" \
  con-name campus-port \
  ipv4.method manual \
  ipv4.addresses "$LOCAL_CIDR" \
  ipv4.gateway "$GATEWAY" \
  ipv4.dns "$DNS_SERVERS" \
  ipv4.route-metric 50 \
  ipv6.method disabled \
  connection.autoconnect yes

sudo nmcli connection up campus-port
```

这里必须填写有线端口的实际地址。认证配置中的 `ip=` 和 `udp-ip=` 不会设置 Linux 网卡地址。

检查：

```bash
ip -brief -4 address show dev "$WIRED_IF"
ip route show default
ip neigh show dev "$WIRED_IF"
```

## 3. 先验证认证和有线公网

```bash
sudo systemctl is-active drcom4scut.service

curl --interface "$WIRED_IF" \
  --ipv4 \
  --connect-timeout 5 \
  --max-time 15 \
  -I https://www.baidu.com/
```

只有有线接口本身已经能够访问公网，热点转发才可能成功。

如果有多个默认路由，可临时明确使用有线接口：

```bash
sudo ip route replace default \
  via "$GATEWAY" \
  dev "$WIRED_IF" \
  src "$LOCAL_IP" \
  metric 50
```

NetworkManager 配置里的 `ipv4.route-metric 50` 用于重启后保持该优先级。

## 4. 检查无线网卡 AP 能力

安装工具：

```bash
sudo apt-get update
sudo apt-get install -y iw network-manager
```

检查：

```bash
iw list | sed -n '/Supported interface modes:/,/Band/p'
```

必须看到：

```text
* AP
```

多数单无线网卡不能同时连接上游 Wi-Fi并创建热点。本方案使用有线网卡作为上游，因此无线网卡可以完整切换为 AP。

## 5. 创建热点

以下示例创建 `Campus-Hotspot`。发布或长期使用前请更换密码：

```bash
sudo nmcli connection delete campus-hotspot 2>/dev/null || true

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
```

2.4 GHz `bg` 兼容性较好。需要 5 GHz 时，应确认无线网卡、驱动和当地法规允许对应频段与信道。

## 6. 开启 IPv4 转发

立即生效：

```bash
sudo sysctl -w net.ipv4.ip_forward=1
```

重启后保持：

```bash
echo 'net.ipv4.ip_forward=1' | \
  sudo tee /etc/sysctl.d/90-campus-hotspot.conf
sudo sysctl --system
```

检查：

```bash
sysctl net.ipv4.ip_forward
```

应显示：

```text
net.ipv4.ip_forward = 1
```

## 7. 启动热点

本地终端操作：

```bash
sudo nmcli connection up campus-hotspot
```

如果当前 SSH 正经过无线网卡，切换成 AP 会断开 SSH。可以使用本地显示器、USB 网络或延迟启动：

```bash
sudo systemd-run \
  --unit=activate-campus-hotspot \
  --on-active=5s \
  /usr/bin/nmcli connection up campus-hotspot
```

随后在客户端连接：

```text
SSID：Campus-Hotspot
密码：创建热点时设置的密码
```

客户端应获得 `10.42.0.x`。热点主机地址为 `10.42.0.1`：

```bash
ssh 用户名@10.42.0.1
```

## 8. 检查转发状态

```bash
ip -brief -4 address show dev "$WIRED_IF"
ip -brief -4 address show dev "$WIFI_IF"
ip route show default
sysctl net.ipv4.ip_forward
sudo systemctl is-active drcom4scut.service
nmcli device status
nmcli connection show --active
```

应满足：

```text
有线接口：实际校园网IPv4
无线接口：10.42.0.1/24
默认路由：经过有线接口和实际网关
IPv4 forwarding：1
drcom4scut.service：active
```

查看 NetworkManager 创建的 NAT/转发规则：

```bash
sudo nft list ruleset
sudo iptables -t nat -S POSTROUTING
sudo iptables -S FORWARD
```

## 9. 客户端测试

在连接热点的客户端执行：

```bash
ping -c 3 10.42.0.1
ping -c 3 223.5.5.5
curl -I --connect-timeout 8 https://www.baidu.com/
```

判断方法：

- 无法访问 `10.42.0.1`：热点、无线连接或 DHCP 存在问题。
- 能访问 `10.42.0.1`，不能访问公网 IP：转发、NAT 或有线上游存在问题。
- 能访问公网 IP，域名失败：DNS 转发存在问题。
- 正常返回 HTTP 状态码：热点共享成功。

## 10. NetworkManager 未自动创建 NAT 时

`ipv4.method shared` 通常会自动创建 DHCP、DNS 和 NAT。若客户端获得了地址但没有公网，可临时补充：

```bash
sudo iptables -t nat -C POSTROUTING \
  -s 10.42.0.0/24 -o "$WIRED_IF" -j MASQUERADE 2>/dev/null || \
sudo iptables -t nat -A POSTROUTING \
  -s 10.42.0.0/24 -o "$WIRED_IF" -j MASQUERADE

sudo iptables -C FORWARD \
  -i "$WIFI_IF" -o "$WIRED_IF" -j ACCEPT 2>/dev/null || \
sudo iptables -A FORWARD \
  -i "$WIFI_IF" -o "$WIRED_IF" -j ACCEPT

sudo iptables -C FORWARD \
  -i "$WIRED_IF" -o "$WIFI_IF" \
  -m conntrack --ctstate ESTABLISHED,RELATED \
  -j ACCEPT 2>/dev/null || \
sudo iptables -A FORWARD \
  -i "$WIRED_IF" -o "$WIFI_IF" \
  -m conntrack --ctstate ESTABLISHED,RELATED \
  -j ACCEPT
```

使用 UFW 时：

```bash
sudo ufw route allow in on "$WIFI_IF" out on "$WIRED_IF"
```

这些是临时诊断规则。优先修复 NetworkManager 的 shared 连接，使规则由 NetworkManager 随连接生命周期管理。

## 11. 停止热点和恢复 Wi-Fi

```bash
sudo nmcli connection down campus-hotspot
```

列出原有 Wi-Fi 连接：

```bash
nmcli connection show
```

恢复某个连接：

```bash
sudo nmcli connection up '原Wi-Fi连接名'
```

禁止热点开机自动启动：

```bash
sudo nmcli connection modify campus-hotspot connection.autoconnect no
```
