#!/usr/bin/env python3
"""主机侧 AT 对接测试：未修改的 WiFiS3（RA4M1 侧）对 bridge 的 at_server 加平台 WiFi 库（BL616CL 侧）。

  run.py --renesas <renesas_uno 包目录> [--platform <arduino-bl 仓库>] [--build <编译目录>]

--renesas 是安装好的 arduino:renesas_uno 包目录（含 libraries/WiFiS3/src 与 cores/arduino），WiFiS3 源码直接从这里编译，
不复制进本仓库。--platform 默认是本仓库旁边的 arduino-bl：取 ArduinoCore-API（cores/bouffalo/api）与 libraries/WiFi。
平台 WiFi 库的 wifi_port、net_port 换成 bl/ 下的主机实现：虚拟 AP、本机 POSIX socket。

两个程序经 socketpair 相连，本脚本另在 127.0.0.1 起回显服务与关闭服务（发 "bye\\n" 后关闭），以及 UDP 回显服务。ra_host 逐项输出结果，
本脚本判定；BL 侧进程必须运行到最后一条 AT+RESTARTBOOTLOADER，由 resetToBootloader 的替身打印标记后退出。另以 bl_host --early-client 单独检查平台 WiFiClient 在 Wi-Fi 初始化前的连接。
退出码 0 表示全部通过。
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.normpath(os.path.join(HERE, "../.."))
SSID = "testnet"
PASS = "pa,ss wo,rd"  # 含逗号与空格：BEGINSTA 以第一个逗号分开网络名与密码
API_SOURCES = ("String.cpp", "Print.cpp", "Stream.cpp", "IPAddress.cpp", "Common.cpp")
WIFIS3_SOURCES = ("Modem.cpp", "WiFi.cpp", "WiFiClient.cpp", "WiFiServer.cpp", "WiFiUdp.cpp", "WiFiSSLClient.cpp",
                  "StringHelpers.cpp")

results = []

# 与 bl/fake_wifi_port.c 的 scan_table 相同：(ssid, bssid, rssi, 信道, wl_enc_type)。第 11 个超出 10 个的上限
SCAN_TABLE = [
    ("testnet", "02:00:00:00:00:AA", -42, 6, 2),
    ("open net", "02:00:00:00:01:02", -50, 1, 5),
    ("abcdefghijklmnopqrstuvwxyz012345", "02:00:00:00:01:03", -55, 11, 4),
    ("wep", "02:00:00:00:01:04", -60, 3, 0),
    ("wpa", "02:00:00:00:01:05", -61, 4, 1),
    ("eap", "02:00:00:00:01:06", -62, 5, 3),
    ("odd", "02:00:00:00:01:07", -63, 13, 255),
    ("n8", "02:00:00:00:01:08", -70, 2, 2),
    ("n9", "02:00:00:00:01:09", -75, 7, 2),
    ("n10", "02:00:00:00:01:0A", -80, 9, 2),
    ("n11", "02:00:00:00:01:0B", -90, 10, 2),
]


def check(name, ok):
    results.append((name, ok))
    print("%s: %s" % (name, "PASS" if ok else "FAIL"), flush=True)
    return ok


def compile_all(out, sources, includes, defines=()):
    objs = []
    for src in sources:
        obj = os.path.join(out, "%d-%s.o" % (len(objs), os.path.basename(src)))
        c = src.endswith(".c") or src.endswith(".impl")
        std = "-std=gnu11" if c else "-std=gnu++17"
        if src.endswith("WiFiS3/src/WiFiSSLClient.cpp"):
            # 该文件 using namespace std，主机 g++ 13 下 std::byte 与 Arduino 的 byte 冲突；它不用 C++17 特性
            std = "-std=gnu++14"
        cmd = ["gcc" if c else "g++", "-c", "-g", "-O1", "-Wall", std]
        cmd += ["-x", "c"] if src.endswith(".impl") else []
        cmd += [] if c else ["-include", "stdlib.h"]  # api/Common.h 声明的 atexit 不带 noexcept，先让 glibc 的声明生效
        cmd += ["-I" + i for i in includes] + ["-D" + d for d in defines] + [src, "-o", obj]
        subprocess.run(cmd, check=True)
        objs.append(obj)
    return objs


def build(args, out):
    api = os.path.join(args.platform, "cores/bouffalo")
    wifi = os.path.join(args.platform, "libraries/WiFi/src")
    wifis3 = os.path.join(args.renesas, "libraries/WiFiS3/src")
    renesas_inc = os.path.join(out, "renesas")  # 只取 FifoBuffer.h，不让 renesas 的 Arduino.h 进入搜索路径
    os.makedirs(renesas_inc, exist_ok=True)
    shutil.copy(os.path.join(args.renesas, "cores/arduino/FifoBuffer.h"), renesas_inc)
    host = os.path.join(HERE, "host")
    common = [os.path.join(host, f) for f in ("host_main.c", "host_arduino.cpp")]
    common += [os.path.join(api, "api/deprecated-avr-comp/avr/dtostrf.c.impl")]
    common += [os.path.join(api, "api", s) for s in API_SOURCES]

    bl_src = common + [os.path.join(HERE, "bl", f) for f in ("bl_main.cpp", "fake_wifi_port.c", "fake_net_port.c",
                                                              "fake_tls_port.c")]
    bl_src += [os.path.join(BRIDGE, "UNOR4USBBridgeBL/at_server.cpp")]
    preferences = os.path.join(args.platform, "libraries/Preferences/src")
    bl_src += [os.path.join(preferences, "Preferences.cpp")]
    bl_src += [os.path.join(wifi, f) for f in ("WiFi.cpp", "WiFiClient.cpp", "WiFiSSLClient.cpp", "WiFiServer.cpp",
                                                  "WiFiUdp.cpp")]
    bl_inc = [host, os.path.join(HERE, "bl"), wifi, os.path.join(BRIDGE, "UNOR4USBBridgeBL"), api, preferences]
    bl_out = os.path.join(out, "bl")
    os.makedirs(bl_out, exist_ok=True)
    bl = os.path.join(out, "bl_host")
    subprocess.run(["g++", "-o", bl] + compile_all(bl_out, bl_src, bl_inc), check=True)

    ra_src = common + [os.path.join(HERE, "ra/ra_main.cpp")] + [os.path.join(wifis3, f) for f in WIFIS3_SOURCES]
    ra_inc = [host, os.path.join(HERE, "bl"), wifis3, api, renesas_inc]
    ra_out = os.path.join(out, "ra")
    os.makedirs(ra_out, exist_ok=True)
    ra = os.path.join(out, "ra_host")
    subprocess.run(["g++", "-o", ra] + compile_all(ra_out, ra_src, ra_inc, ["ARDUINO_UNOWIFIR4"]), check=True)
    return bl, ra


class Servers:
    """回显服务记录每条连接收到的字节；关闭服务发 "bye\\n" 后关闭。
    UDP 回显服务把数据报原样发回来源并记录长度；收到 b"push" 时改从另一个 socket 发 b"pushed"（端口为 push_port）"""

    def __init__(self):
        self.received = []
        self.echo = self._listen(self._echo)
        self.close = self._listen(self._close)
        self.udp_lens = []
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", 0))
        self.udp_port = self.udp.getsockname()[1]
        self.push = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.push.bind(("127.0.0.1", 0))
        self.push_port = self.push.getsockname()[1]
        threading.Thread(target=self._udp_echo, daemon=True).start()

    def _udp_echo(self):
        while True:
            data, addr = self.udp.recvfrom(65536)
            self.udp_lens.append(len(data))
            (self.push if data == b"push" else self.udp).sendto(b"pushed" if data == b"push" else data, addr)

    def _listen(self, handler):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        s.listen(16)
        threading.Thread(target=self._accept, args=(s, handler), daemon=True).start()
        return s.getsockname()[1]

    def _accept(self, s, handler):
        while True:
            conn, _ = s.accept()
            threading.Thread(target=handler, args=(conn,), daemon=True).start()

    def _echo(self, conn):
        data = b""
        conn.settimeout(30)
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
                conn.sendall(chunk)
        except OSError:
            pass
        conn.close()
        self.received.append(data)

    def _close(self, conn):
        conn.sendall(b"bye\n")
        conn.close()


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def server_peer(port, log):
    """按 ra_main.cpp server_tests 的顺序当客户端，每步等到应答再走下一步；结果记进 log"""

    def connect(timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                return socket.create_connection(("127.0.0.1", port), timeout=5)
            except OSError:
                time.sleep(0.05)
        raise TimeoutError("connect %d" % port)

    def line(s):
        buf = b""
        while not buf.endswith(b"\n"):
            ch = s.recv(1)
            if not ch:
                break
            buf += ch
        return buf

    try:
        a = connect(150)  # ra 跑完前面各项才 begin
        a.sendall(b"hello\n")
        log["chat"] = [line(a)]
        a.sendall(b"again\n")
        log["chat"].append(line(a))
        a.close()
        b = connect()
        log["welcome"] = line(b)
        b.sendall(b"ok\n")
        log["accept_eof"] = b.recv(16) == b""
        b.close()
        c1, c2 = connect(), connect()
        c1.sendall(b"x\n")
        c2.sendall(b"x\n")
        log["bc"] = [line(c1), line(c2)]
        c1.close()
        c2.close()
        log["churn"] = 0
        for _ in range(10):
            s = connect()
            s.sendall(b"n\n")
            log["churn"] += line(s) == b"k\n"
            s.close()
        time.sleep(1.0)
        try:
            socket.create_connection(("127.0.0.1", port), timeout=2).close()
            log["after_end"] = "connected"
        except OSError:
            log["after_end"] = "refused"
    except Exception as e:  # noqa: BLE001
        log["error"] = repr(e)


def fields(lines, item):
    for ln in lines:
        parts = ln.split(" ")
        if parts[0] == item:
            return parts[1:]
    return None


def nums(lines, item):
    try:
        return [int(x) for x in fields(lines, item)]
    except (TypeError, ValueError):
        return None


def payload(n):
    return bytes((i * 31 + n) & 0xFF for i in range(n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--renesas", required=True)
    ap.add_argument("--platform", default=os.path.normpath(os.path.join(BRIDGE, "../arduino-bl")))
    ap.add_argument("--build")
    args = ap.parse_args()
    out = args.build or tempfile.mkdtemp(prefix="at_host-")
    bl, ra = build(args, out)

    servers = Servers()
    early = subprocess.run([bl, "--early-client", str(servers.echo)], capture_output=True, text=True, timeout=30)
    print(early.stdout + early.stderr, end="")
    check("平台 WiFiClient 在 Wi-Fi 初始化前连接直接失败、不调用 net_port（rc=%d，%s）"
          % (early.returncode, early.stdout.strip()), early.returncode == 0 and early.stdout.strip() == "EARLYCLIENT 0 0")

    udp_local = free_port(socket.SOCK_DGRAM)  # 给 BL 侧绑定的本机 UDP 端口
    server_ports = [free_port() for _ in range(6)]
    peer = {}
    peer_thread = threading.Thread(target=server_peer, args=(server_ports[0], peer), daemon=True)
    peer_thread.start()

    a, b = socket.socketpair()
    env = dict(os.environ, AT_HOST_SSID=SSID, AT_HOST_PASS=PASS)
    bl_err = open(os.path.join(out, "bl_host.stderr"), "w")
    blp = subprocess.Popen([bl, str(b.fileno())], pass_fds=(b.fileno(),), env=env, stderr=bl_err)
    start = time.monotonic()
    rap = subprocess.run([ra, str(a.fileno()), SSID, PASS, str(servers.echo), str(servers.close), str(servers.udp_port),
                          str(udp_local), ",".join(map(str, server_ports))],
                         pass_fds=(a.fileno(),), capture_output=True, text=True, timeout=180)
    took = time.monotonic() - start
    try:
        bl_rc = blp.wait(timeout=5)
    except subprocess.TimeoutExpired:
        blp.terminate()
        bl_rc = blp.wait()
    bl_err.close()
    time.sleep(0.2)

    print(rap.stdout, end="")
    if rap.stderr:
        print("ra_host stderr:\n" + rap.stderr, end="")
    with open(os.path.join(out, "bl_host.stderr")) as f:
        err = f.read()
    if err:
        print("bl_host stderr:\n" + err, end="")
    lines = [ln.strip() for ln in rap.stdout.splitlines() if ln.strip()]
    v = lambda item: fields(lines, item)  # noqa: E731
    n = lambda item: nums(lines, item)  # noqa: E731

    check("ra_host 输出到 DONE（%.1f s）" % took, "DONE" in lines)
    check("BL 侧进程运行到 RESTARTBOOTLOADER，调用 resetToBootloader 后退出（rc=%s）" % bl_rc,
          bl_rc == 0 and err.count("RESETTOBOOTLOADER") == 1)
    check("STATUS0 为 0 且 2 s 内返回（SOFTRESETWIFI 握手，%s）" % v("STATUS0"),
          n("STATUS0") is not None and n("STATUS0")[0] == 0 and n("STATUS0")[1] < 2000)
    check("FW 为 0.1.0（%s）" % v("FW"), v("FW") == ["0.1.0"])
    check("firmwareVersionU32 为 0x000100（%s）" % v("FWU32"), v("FWU32") == ["000100"])
    check("联网前连接失败（%s）" % v("EARLYCONNECT"), n("EARLYCONNECT") == [0])
    check("联网前解析失败（%s）" % v("EARLYDNS"), n("EARLYDNS") == [0])
    check("联网前 ping 返回负值（%s）" % v("EARLYPING"), n("EARLYPING") is not None and n("EARLYPING")[0] < 0)
    rows = [ln.split(" ", 1)[1].split("|") for ln in lines if ln.startswith("SCANAP ")]
    got = [(r[1], r[2], int(r[3]), int(r[4]), int(r[5])) for r in rows]
    check("扫描：未连接时可用，按 RSSI 保留前 10 个，SSID 含空格与 32 字节、各加密方式往返一致（%s，%d 行）"
          % (v("SCAN"), len(got)), n("SCAN") is not None and n("SCAN")[0] == 10 and got == SCAN_TABLE[:10])
    e = n("SCANEMPTY")
    check("扫描到 0 个网络时 1 s 内返回 0（%s）" % e, e is not None and e[0] == 0 and e[1] < 1000)
    f = n("SCANFAIL")
    check("扫描失败时 1 s 内返回 0（%s）" % f, f is not None and f[0] == 0 and f[1] < 1000)
    check("错误密码：begin 失败，status 为 WL_CONNECT_FAILED（%s）" % v("BADPASS"), n("BADPASS") == [4, 4])
    check("不存在的网络：begin 失败，status 为 WL_NO_SSID_AVAIL（%s）" % v("NOSSID"), n("NOSSID") == [4, 1])
    check("含逗号与空格的密码连接成功（%s）" % v("BEGIN"), n("BEGIN") is not None and n("BEGIN")[0] == 3)
    check("地址、网关、掩码、DNS（%s %s %s %s）" % (v("IP"), v("GW"), v("MASK"), v("DNS")),
          v("IP") == ["127.0.0.1"] and v("GW") == ["127.0.0.1"] and v("MASK") == ["255.0.0.0"]
          and v("DNS") == ["127.0.0.53"])
    check("SSID、RSSI、BSSID、MAC（%s %s %s %s）" % (v("SSID"), v("RSSI"), v("BSSID"), v("MAC")),
          v("SSID") == [SSID] and v("RSSI") == ["-42"] and v("BSSID") == ["02:00:00:00:00:AA"]
          and v("MAC") == ["02:00:00:00:00:01"])
    check("域名解析成功与失败（%s，%s）" % (v("HOSTBYNAME"), v("HOSTBYNAMEBAD")),
          v("HOSTBYNAME") == ["1", "127.0.0.1"] and n("HOSTBYNAMEBAD") == [0])
    p = n("PING")
    check("ping 地址、域名成功，未知域名失败（%s）" % p, p is not None and p[0] >= 0 and p[1] >= 0 and p[2] < 0)
    check("按地址连接，3000 字节二进制回显一致（%s）" % v("ECHOIP"), n("ECHOIP") == [1, 3000, 3000, 1])
    check("回显服务收到的 3000 字节与应发负载一致", payload(3000) in servers.received)
    r = v("REMOTE")
    check("remoteIP、remotePort（%s）" % r, r is not None and r[0] == "127.0.0.1" and r[1] == r[2])
    check("按域名连接，100 字节回显一致（%s）" % v("ECHONAME"), n("ECHONAME") == [1, 100, 100, 1])
    check("setConnectionTimeout 后的连接命令可用（%s）" % v("ECHOTIMEOUT"), n("ECHOTIMEOUT") == [1, 1, 1, 1])
    check("对端关闭后读完数据、connected() 为 0（%s）" % v("PEERCLOSE"), n("PEERCLOSE") == [1, 4, 0])
    check("8 个 socket 成功、第 9 个失败、释放后可再用（%s）" % v("SOCKETS"), n("SOCKETS") == [8, 0, 1])
    check("TLS：按域名用根证书束连接，3000 字节二进制回显一致（%s）" % v("SSLECHO"), n("SSLECHO") == [1, 3000, 3000, 1])
    r = v("SSLREMOTE")
    check("TLS remoteIP、remotePort（%s）" % r, r is not None and r[0] == "127.0.0.1" and r[1] == r[2])
    check("TLS available 后 peek 为首字节 'p'，read 读回 2 字节（%s）" % v("SSLPEEK"), n("SSLPEEK") == [112, 2, 1])
    check("TLS stop 后 connected() 为 0（%s）" % v("SSLSTOP"), n("SSLSTOP") == [0])
    check("TLS 按地址连接时根证书束不认，连接失败（%s）" % v("SSLUNTRUSTED"), n("SSLUNTRUSTED") == [0])
    check("setCACert 的 PEM 逐字节传到平台，连接成功、100 字节回显一致（%s）" % v("SSLCA"), n("SSLCA") == [1, 100, 100, 1])
    check("错误的 CA 连接失败；setCACert(nullptr) 后恢复用根证书束（%s）" % v("SSLCAFLOW"), n("SSLCAFLOW") == [0, 1])
    check("setEccSlot 未预置私钥槽时失败，载荷读完、之后带超时的连接正常（%s %s）" % (v("SSLECC"), v("SSLTIMEOUT")),
          n("SSLECC") == [0] and n("SSLTIMEOUT") == [1, 1, 1, 1])
    check("TLS 对端关闭后读完数据、connected() 为 0（%s）" % v("SSLPEERCLOSE"), n("SSLPEERCLOSE") == [1, 4, 0])
    check("SSL 编号不能用于 CLIENT 命令，TCP 编号不能用于 SSL 命令（%s）" % v("SSLMIX"), n("SSLMIX") == [1, 0, 0])
    check("8 个 TLS 连接成功、第 9 个失败、释放后可再用（%s）" % v("SSLSOCKETS"), n("SSLSOCKETS") == [8, 0, 1])
    check("联网前 WiFiUDP.begin 失败（%s）" % v("EARLYUDP"), n("EARLYUDP") == [0])
    check("UDP：begin、beginPacket、分两次 write 1400 字节（含 0x00、CRLF、'|'）、endPacket，回显 1400 字节，"
          "peek 为首字节、available 为全长、每次 500 字节读回一致（%s）" % v("UDPECHO"),
          n("UDPECHO") == [1, 1, 1400, 1, 1400, 0, 1400, 1400, 1])
    r = v("UDPREMOTE")
    check("UDP remoteIP、remotePort（%s）" % r, r is not None and r[0] == "127.0.0.1" and r[1] == r[2])
    u = n("UDPNONE")
    check("没有数据报时 parsePacket 1 s 内返回 0（%s）" % u, u is not None and u[0] == 0 and u[1] < 1000)
    check("UDP 按域名 beginPacket，flush 后 available 为 0（%s）" % v("UDPNAME"), n("UDPNAME") == [1, 1, 5, 0])
    u = n("UDPPUSH")
    check("收到另一来源端口的数据报，remotePort 为该端口（%s，期望 %d）" % (u, servers.push_port),
          u == [6, 1, servers.push_port])
    check("超过 1460 字节的 write 失败、帧保持同步，已缓冲的 1460 字节照常发出并回显一致（%s）" % v("UDPBIG"),
          n("UDPBIG") == [1, 0, 1, 1460, 1])
    check("UDP 回显服务收到的长度依次为 1400、5、4、1460（%s）" % servers.udp_lens, servers.udp_lens == [1400, 5, 4, 1460])
    check("4 个 UDP 成功、第 5 个失败、释放后可再用（%s）" % v("UDPSOCKETS"), n("UDPSOCKETS") == [4, 0, 1])
    u = n("UDPMULTI")
    check("组播 beginMulticast 在fake IGMP下1 s内成功（%s）" % u, u is not None and u[0] == 1 and u[1] < 1000)
    peer_thread.join(timeout=5)
    check("联网前 WiFiServer.begin 失败（%s）" % v("EARLYSERVER"), n("EARLYSERVER") == [0])
    check("服务端 begin（%s）；客户端线程无异常（%s）" % (v("SRVBEGIN"), peer.get("error")),
          n("SRVBEGIN") == [1] and "error" not in peer)
    check("available() 两次返回同一连接，逐行应答（%s，%s）" % (v("SRVCHAT"), peer.get("chat")),
          n("SRVCHAT") == [2, 1] and peer.get("chat") == [b"HELLO\n", b"AGAIN\n"])
    check("accept() 取得新连接，收发后 stop() 关闭（%s，%s）" % (v("SRVACCEPT"), peer.get("welcome")),
          n("SRVACCEPT") == [1, 1] and peer.get("welcome") == b"welcome\n" and peer.get("accept_eof") is True)
    check("server.write 发给 available() 收下的两个连接（%s，%s）" % (v("SRVBCAST"), peer.get("bc")),
          n("SRVBCAST") == [2, 3] and peer.get("bc") == [b"bc\n", b"bc\n"])
    check("连续 10 个连接（多于 8 个位置）都得到应答，断开的位置被回收（%s，%s）" % (v("SRVCHURN"), peer.get("churn")),
          n("SRVCHURN") == [10] and peer.get("churn") == 10)
    check("end() 后不再接受连接（%s，%s）" % (v("SRVEND"), peer.get("after_end")),
          n("SRVEND") == [0] and peer.get("after_end") == "refused")
    check("4 个服务端成功、第 5 个失败、释放后可再用（%s）" % v("SRVLIMIT"), n("SRVLIMIT") == [4, 0, 1])
    check("已连接时 config 立即生效：地址、网关、掩码、DNS 与参数相同，状态 3（%s）" % v("SETIP"),
          v("SETIP") == ["127.0.0.2", "127.0.0.4", "255.255.0.0", "127.0.0.3", "3"])
    check("setDNS 两个 DNS，地址不变（%s）" % v("SETDNS"), v("SETDNS") == ["127.0.0.2", "127.0.0.5", "127.0.0.6"])
    check("断开后再 begin 仍用静态地址与 DNS（%s）" % v("SETIPAGAIN"), v("SETIPAGAIN") == ["3", "127.0.0.2", "127.0.0.5"])
    check("config(地址)：网关 x.y.z.1、/24、DNS0 用网关，DNS1 不变（%s）" % v("SETIP1"),
          v("SETIP1") == ["127.0.0.9", "127.0.0.1", "255.255.255.0", "127.0.0.1", "127.0.0.6"])
    check("静态地址时 TCP 照常（%s）" % v("STATICECHO"), n("STATICECHO") == [1, 100, 100, 1])
    check("SETIP 参数不足或地址不合法时 ERROR（%s）" % v("SETIPBAD"), n("SETIPBAD") == [0, 0])
    check("config(0.0.0.0) 恢复 DHCP 的地址与 DNS（%s）" % v("SETIPDHCP"),
          v("SETIPDHCP") == ["127.0.0.1", "127.0.0.1", "127.0.0.53"])
    check("SOFTRESETWIFI 恢复 DHCP（%s）" % v("SOFTRESETDHCP"), v("SOFTRESETDHCP") == ["1", "3", "127.0.0.1"])
    check("已连 STA 时 beginAP：返回与状态为 7，localIP、掩码、网关、softAPIP 为默认 192.168.4.1/24（%s）" % v("APBEGIN"),
          v("APBEGIN") == ["7", "7", "192.168.4.1", "255.255.255.0", "192.168.4.1", "192.168.4.1"])
    check("AP 模式下 SSID()、softAPSSID() 为 AP 的名称，macAddress 走 MACSOFTAP（%s %s %s）"
          % (v("APSSID"), v("APSOFTSSID"), v("APMAC")),
          v("APSSID") == ["bl-ap"] and v("APSOFTSSID") == ["bl-ap"] and v("APMAC") == ["02:00:00:00:00:01"])
    check("beginAP 已断开 STA：DNS、RSSI 为 0（%s）" % v("APSTA"), v("APSTA") == ["0.0.0.0", "0"])
    check("AP 运行时服务端 begin、按地址连接、accept 后读到数据（%s）" % v("APSERVER"), n("APSERVER") == [1, 1, ord("x")])
    check("AP 运行时 WiFiUDP.begin 成功（%s）" % v("APUDP"), n("APUDP") == [1])
    check("config(地址) 后 beginAP：AP 用该地址与 /24（%s，%s）" % (v("APCONFIG"), v("APSSID2")),
          v("APCONFIG") == ["7", "7", "192.48.56.2", "255.255.255.0"] and v("APSSID2") == ["bl-ap2"])
    check("beginAP(ssid) 开放网络（%s）" % v("APOPEN"), n("APOPEN") == [7, 7])
    check("密码过短、信道 14：返回 WL_AP_FAILED，运行中的 AP 不受影响（%s）" % v("APBAD"), n("APBAD") == [9, 9, 7])
    check("底层启动失败：返回与状态为 WL_AP_FAILED（%s）" % v("APFAIL"), n("APFAIL") == [9, 9])
    check("BEGINSOFTAP 接受隐藏 SSID 参数、SOFTAPCONFIG 参数不足被拒绝（%s）" % v("APERR"), n("APERR") == [1, 0])
    check("AP 运行时 begin：停止 AP，STA 用 DHCP 连上（%s）" % v("APTOSTA"),
          v("APTOSTA") == ["3", "3", "127.0.0.1", "0.0.0.0"])
    check("end（SOFTRESETWIFI）停止 AP（%s）" % v("APEND"), v("APEND") == ["7", "6", "0.0.0.0"])
    u = n("UNKNOWN")
    check("未知命令 1 s 内失败（%s）" % u, u is not None and u[0] == 0 and u[1] < 1000)
    check("disconnect 后 status 为 WL_DISCONNECTED（%s）" % v("DISC"), n("DISC") == [6])
    r = n("RESTARTBOOT")
    check("RESTARTBOOTLOADER 复位前应答 OK、1 s 内返回（%s）" % r, r is not None and r[0] == 1 and r[1] < 1000)

    failed = [name for name, ok in results if not ok]
    print("%d 项检查，%d 项失败；编译目录 %s" % (len(results), len(failed), out))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
