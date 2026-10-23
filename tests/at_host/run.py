#!/usr/bin/env python3
"""主机侧 AT 对接测试：未修改的 WiFiS3（RA4M1 侧）对 bridge 的 at_server 加平台 WiFi 库（BL616CL 侧）。

  run.py --renesas <renesas_uno 包目录> [--platform <arduino-bl 仓库>] [--build <编译目录>]

--renesas 是安装好的 arduino:renesas_uno 包目录（含 libraries/WiFiS3/src 与 cores/arduino），WiFiS3 源码直接从这里编译，
不复制进本仓库。--platform 默认是本仓库旁边的 arduino-bl：取 ArduinoCore-API（cores/bouffalo/api）与 libraries/WiFi。
平台 WiFi 库的 wifi_port、net_port 换成 bl/ 下的主机实现：虚拟 AP、本机 POSIX socket。

两个程序经 socketpair 相连，本脚本另在 127.0.0.1 起回显服务与关闭服务（发 "bye\\n" 后关闭）。ra_host 逐项输出结果，
本脚本判定；BL 侧进程必须一直存活。另以 bl_host --early-client 单独检查平台 WiFiClient 在 Wi-Fi 初始化前的连接。
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
WIFIS3_SOURCES = ("Modem.cpp", "WiFi.cpp", "WiFiClient.cpp", "StringHelpers.cpp")

results = []


def check(name, ok):
    results.append((name, ok))
    print("%s: %s" % (name, "PASS" if ok else "FAIL"), flush=True)
    return ok


def compile_all(out, sources, includes, defines=()):
    objs = []
    for src in sources:
        obj = os.path.join(out, "%d-%s.o" % (len(objs), os.path.basename(src)))
        c = src.endswith(".c") or src.endswith(".impl")
        cmd = ["gcc" if c else "g++", "-c", "-g", "-O1", "-Wall", "-std=gnu11" if c else "-std=gnu++17"]
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

    bl_src = common + [os.path.join(HERE, "bl", f) for f in ("bl_main.cpp", "fake_wifi_port.c", "fake_net_port.c")]
    bl_src += [os.path.join(BRIDGE, "UNOR4USBBridgeBL/at_server.cpp")]
    bl_src += [os.path.join(wifi, f) for f in ("WiFi.cpp", "WiFiClient.cpp")]
    bl_inc = [host, os.path.join(HERE, "bl"), wifi, os.path.join(BRIDGE, "UNOR4USBBridgeBL"), api]
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
    """回显服务记录每条连接收到的字节；关闭服务发 "bye\\n" 后关闭"""

    def __init__(self):
        self.received = []
        self.echo = self._listen(self._echo)
        self.close = self._listen(self._close)

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

    a, b = socket.socketpair()
    env = dict(os.environ, AT_HOST_SSID=SSID, AT_HOST_PASS=PASS)
    bl_err = open(os.path.join(out, "bl_host.stderr"), "w")
    blp = subprocess.Popen([bl, str(b.fileno())], pass_fds=(b.fileno(),), env=env, stderr=bl_err)
    start = time.monotonic()
    rap = subprocess.run([ra, str(a.fileno()), SSID, PASS, str(servers.echo), str(servers.close)],
                         pass_fds=(a.fileno(),), capture_output=True, text=True, timeout=180)
    took = time.monotonic() - start
    bl_alive = blp.poll() is None
    blp.terminate()
    blp.wait()
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
    check("BL 侧进程全程存活", bl_alive)
    check("STATUS0 为 0 且 2 s 内返回（SOFTRESETWIFI 握手，%s）" % v("STATUS0"),
          n("STATUS0") is not None and n("STATUS0")[0] == 0 and n("STATUS0")[1] < 2000)
    check("FW 为 0.1.0（%s）" % v("FW"), v("FW") == ["0.1.0"])
    check("联网前连接失败（%s）" % v("EARLYCONNECT"), n("EARLYCONNECT") == [0])
    check("联网前解析失败（%s）" % v("EARLYDNS"), n("EARLYDNS") == [0])
    check("联网前 ping 返回负值（%s）" % v("EARLYPING"), n("EARLYPING") is not None and n("EARLYPING")[0] < 0)
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
    u = n("UNKNOWN")
    check("未知命令 1 s 内失败（%s）" % u, u is not None and u[0] == 0 and u[1] < 1000)
    check("disconnect 后 status 为 WL_DISCONNECTED（%s）" % v("DISC"), n("DISC") == [6])

    failed = [name for name, ok in results if not ok]
    print("%d 项检查，%d 项失败；编译目录 %s" % (len(results), len(failed), out))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
