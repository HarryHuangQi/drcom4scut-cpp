# 协议实现说明

## EAP 阶段

客户端通过 Linux `AF_PACKET` 在指定物理接口上收发 EtherType `0x888e`：

1. EAPOL Start。
2. EAP Identity Response。
3. EAP-MD5 Response，摘要为 `MD5(identifier + password + challenge)`。
4. EAP Success 后进入 DrCOM UDP 阶段。

`windows-31` 在 Identity/MD5 扩展字段中使用客户端标志 `0x31`。`legacy-2a` 保留旧格式兼容，但不包含当前 Windows 流程的全部字段。

## UDP 阶段

当前实现的 `windows-31` 流程：

1. MiscAlive。
2. 260 字节 MiscInfo，包括认证 MAC、UDP 载荷 IPv4、网关、两个 DNS、客户端版本区和 16 字节尾部。
3. MiscResponseInfo 建立会话。
4. 等待约 1.4 秒，发送 40 字节启动心跳。
5. 收到启动心跳响应后进入 Ready。
6. 按 `heartbeat-interval` 发送常规心跳，同时维持 EAP Identity 心跳。

MiscInfo 校验覆盖完整报文长度。启动心跳与常规 Alive 是不同报文，不能互相替代。

## 地址模型

实现明确区分：

- `LocalIPv4`：Linux 物理接口的真实地址，也是 UDP socket 的实际源地址。
- `AuthIPv4`：写入 EAP Identity 和 EAP-MD5 的地址。
- `UdpIPv4`：写入 MiscInfo 和 Heartbeat3 的地址。

不同校园网络可能要求三者相同，也可能允许或要求认证载荷地址与接口地址分离。

## 测试

`tests/protocol_tests.cpp` 验证报文长度、字段、校验和和固定向量。`tests/linux_integration.py` 使用 network namespace、veth 和模拟认证端验证真实 Linux 网络收发和状态机。
