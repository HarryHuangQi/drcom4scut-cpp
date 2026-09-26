#!/usr/bin/env python3
"""Root-only isolated Linux integration test. No campus credentials/network used.

Creates one temporary network namespace and veth pair. Exercises the actual
C++ AF_PACKET/UDP sockets against this independent synthetic server, then
deletes only the namespace/link created by this test.
"""
import hashlib
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def cmd(*args):
    return subprocess.check_output(args, universal_newlines=True)


def check(condition, name):
    if not condition:
        raise AssertionError(name)


def run(binary):
    check(os.geteuid() == 0, "run as root (isolated network namespace needs CAP_NET_ADMIN)")
    check(shutil.which("ip"), "install iproute2")
    namespace = f"scut-test-{os.getpid()}"
    peer = f"scut{os.getpid()}"[:15]
    client_link = f"scli{os.getpid()}"[:15]
    server_ip, client_ip = "192.0.2.2", "192.0.2.1"
    auth_ip = "198.51.100.77"  # Deliberately differs from the interface address.
    client_mac = bytes.fromhex("020000000001")
    server_mac = bytes.fromhex("020000000002")
    proc = None
    created = False
    link_created = False
    try:
        cmd("ip", "netns", "add", namespace)
        created = True
        cmd("ip", "link", "add", peer, "type", "veth", "peer", "name", client_link)
        link_created = True
        cmd("ip", "link", "set", client_link, "netns", namespace)
        cmd("ip", "link", "set", peer, "address", "02:00:00:00:00:02")
        cmd("ip", "addr", "add", server_ip + "/30", "dev", peer)
        cmd("ip", "link", "set", peer, "up")
        cmd("ip", "-n", namespace, "link", "set", "lo", "up")
        cmd("ip", "-n", namespace, "link", "set", client_link, "address", "02:00:00:00:00:01")
        cmd("ip", "-n", namespace, "addr", "add", client_ip + "/30", "dev", client_link)
        cmd("ip", "-n", namespace, "link", "set", client_link, "up")
        with tempfile.TemporaryDirectory(prefix="scut-test-") as temp:
            config = Path(temp) / "client.conf"
            config.write_text(f"interface={client_link}\nprofile=windows-31\nip={auth_ip}\n"
                              f"gateway={server_ip}\nmac=02:00:00:00:00:01\n"
                              "username=student\npassword=qwert12345\n"
                              f"host=scut.test\nudp-local-port=61440\ndns={server_ip},202.112.17.33\n"
                              "interval=300\nretry=2\nheartbeat-interval=1\n"
                              "udp-timeout=1\neap-timeout=2\nrun-seconds=12\n")
            raw = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x888e))
            raw.bind((peer, 0))
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.bind((server_ip, 61440))
            dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            dns.bind((server_ip, 53))
            challenge = bytes.fromhex("ff62b079ca26d283ca26d28300000000")
            digest = hashlib.md5(b"\0qwert12345" + challenge).digest()
            flux = bytes.fromhex("3bab4e04")
            modified = b""
            identity_count = 0
            heartbeat_count = 0
            alive_requests = 0
            dropped_periodic = False
            eap_heartbeat = False
            did_dns = False
            startup_heartbeat = False

            def eap(code, identifier, payload=b""):
                body = struct.pack("!BBH", code, identifier, 4 + len(payload)) + payload
                packet = client_mac + server_mac + struct.pack("!HBBH", 0x888e, 1, 0, len(body)) + body
                raw.send(packet.ljust(96, b"\0"))

            def launch():
                return subprocess.Popen(["ip", "netns", "exec", namespace, binary, "--config", str(config), "--once"],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)

            proc = launch()
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and proc.poll() is None:
                ready, _, _ = select.select([raw, udp, dns], [], [], 0.1)
                for sock in ready:
                    data, addr = sock.recvfrom(4096)
                    if sock is raw:
                        if len(data) < 18 or data[6:12] != client_mac:
                            continue
                        if data[15] == 1:
                            # Malformed request must be ignored, then valid identity begins.
                            eap(1, 1, b"\x04\x10\x01")
                            eap(1, 1, b"\x01")
                        elif data[15] == 0 and len(data) >= 23:
                            if data[22] == 1:
                                check(data[23:39] == b"student\x00Da\x00\x00" + socket.inet_aton(auth_ip), "Identity auth IP")
                                if data[19] == 9:
                                    eap_heartbeat = True
                                else:
                                    identity_count += 1
                                    if identity_count >= 2:
                                        eap(1, 0, b"\x04\x10" + challenge)
                            elif data[22] == 4:
                                check(data[23:40] == b"\x10" + digest, "EAP MD5 response")
                                check(data[47:52] == b"\x00Da1\x00", "Windows-31 EAP version")
                                check(data[52:56] == socket.inet_aton(auth_ip), "EAP MD5 auth IP")
                                eap(3, 0)
                    elif sock is dns:
                        check(data[12:-4] == b"\x04scut\x04test\0", "DNS question")
                        reply = data[:2] + bytes.fromhex("81800001000100000000") + data[12:]
                        reply += bytes.fromhex("c00c000100010000003c0004") + socket.inet_aton(server_ip)
                        dns.sendto(reply, addr)
                        did_dns = True
                    elif data == bytes.fromhex("0700080001000000"):
                        udp.sendto(bytes.fromhex("07000c0002"), addr)  # too short; must be ignored
                        udp.sendto(bytes.fromhex("07000c0002000000") + flux, addr)
                    elif len(data) == 260 and data[4] == 3:
                        check(data[6:12] == client_mac and data[12:16] == socket.inet_aton(client_ip), "MiscInfo local IP")
                        check(data[32:39] == b"student", "MiscInfo user")
                        check(data[:5] == bytes.fromhex("0701040103") and data[16:20] == bytes.fromhex("02220031"),
                              "Windows-31 MiscInfo header")
                        check(data[76:80] == socket.inet_aton(server_ip), "MiscInfo primary DNS")
                        check(data[80:84] == socket.inet_aton(server_ip), "MiscInfo gateway")
                        check(data[84:88] == socket.inet_aton("202.112.17.33"), "MiscInfo secondary DNS")
                        check(data[116:125] == bytes.fromhex("4472434f4d00960231"), "MiscInfo version")
                        check(data[180:220] == b"b1b54da1eb64ed14592830f7f895c373d507472a", "MiscInfo client hash")
                        check(data[244:260] == bytes.fromhex("2001025030007004aa0cb7dee93f3c65"), "MiscInfo trailer")
                        checksum_input = bytearray(data)
                        checksum_input[24:28] = bytes.fromhex("c72f3101")
                        checksum_input[28] = 126
                        checksum = 0
                        for word in struct.unpack("<65I", checksum_input):
                            checksum ^= word
                        check(data[24:28] == struct.pack("<I", (checksum * 19680126) & 0xffffffff), "MiscInfo checksum")
                        modified = data[24:28] + digest[4:]
                        reply = bytearray(32)
                        reply[0], reply[4] = 7, 4
                        reply[16:32] = bytes.fromhex("4439d8edac314b07dd8c5f3bef0f04d8")
                        udp.sendto(reply, addr)
                    elif len(data) == 38 and data[0] == 255:
                        check(data[1:17] == modified, "Alive patched MD5")
                        check(data[20:36].hex() == "4472636fca26d283dd197dd9fee1016c", "Alive decrypted token")
                        alive_requests += 1
                        # Drop one complete periodic cycle (initial send + two retries).
                        # The established EAP session must survive this transient UDP loss.
                        if alive_requests <= 3:
                            if alive_requests == 3:
                                dropped_periodic = True
                            continue
                        udp.sendto(bytes.fromhex("0700080006000000"), addr)
                    elif len(data) == 40 and data[:8] == bytes.fromhex("070028000b010f27"):
                        startup_heartbeat = True
                        reply = bytearray(272)
                        reply[:8] = bytes.fromhex("070010010b06dc02")
                        reply[8:10] = data[8:10]
                        udp.sendto(reply, addr)
                    elif len(data) == 40 and data[4:6] == b"\x0b\x01":
                        reply = bytearray(40)
                        reply[:6] = bytes([7, data[1], 40, 0, 11, 2])
                        reply[16:20] = bytes.fromhex("67513f04")
                        udp.sendto(reply, addr)
                    elif len(data) == 40 and data[4:6] == b"\x0b\x03":
                        check(data[16:20] == bytes.fromhex("67513f04"), "Heartbeat flux")
                        check(data[28:32] == socket.inet_aton(client_ip), "Heartbeat local IP")
                        checksum_input = bytearray(data)
                        checksum_input[24:28] = b"\0" * 4
                        checksum = 0
                        for word in struct.unpack("<20H", checksum_input):
                            checksum ^= word
                        check(data[24:28] == struct.pack("<I", checksum * 711), "Heartbeat checksum")
                        udp.sendto(bytes([7, data[1], 40, 0, 11, 4]) + b"\0" * 34, addr)
                        heartbeat_count += 1
                        eap(1, 9, b"\x01")
                    else:
                        raise AssertionError(f"unexpected UDP packet {data.hex()}")
                if heartbeat_count >= 2 and eap_heartbeat:
                    proc.send_signal(signal.SIGTERM)
                    break
            output = proc.communicate(timeout=5)[0]
            print(output)
            check(proc.returncode == 0, "client successful SIGTERM exit")
            check(identity_count >= 2 and heartbeat_count >= 2 and eap_heartbeat and did_dns and startup_heartbeat,
                  "retry, DNS, startup heartbeat, EAP heartbeat and UDP cycles all completed")
            check(dropped_periodic and "UDP heartbeat reply timeout; ignored (1/3)" in output,
                  "one lost periodic UDP cycle does not reconnect the EAP session")
            check("UDP heartbeat complete" in output, "client accepted Heartbeat4")
            check("LocalIPv4=" + client_ip + " AuthIPv4=" + auth_ip + " UdpIPv4=" + client_ip in output,
                  "local, EAP authentication and UDP IPv4 are reported separately")

            # No server responses: bounded retries must end with a failure exit.
            proc = launch()
            output = proc.communicate(timeout=8)[0]
            print(output)
            check(proc.returncode == 1 and "EAP reply timeout" in output, "bounded timeout exit")
            raw.close(); udp.close(); dns.close()
            print("PASS: Linux AF_PACKET + DNS + EAP-MD5 + UDP handshake/heartbeat + retry + SIGTERM + timeout")
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill(); proc.wait()
        if created:
            subprocess.run(["ip", "netns", "del", namespace], check=False)
        if link_created:
            subprocess.run(["ip", "link", "del", peer], check=False, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    run(str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/drcom4scut").resolve()))
