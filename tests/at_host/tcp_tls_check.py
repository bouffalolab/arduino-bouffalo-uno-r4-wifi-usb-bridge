#!/usr/bin/env python3
"""Software/fake TCP/TLS AT contract checks; real TLS/RF acceptance is separate.
Run run.py first, then pass its bl_host binary.
"""
import argparse
import os
import socket
import subprocess
import threading
import time
from pathlib import Path


BL = None
CHECKS = 0


def read_until(sock, token=b"OK\r\n", timeout=5):
    sock.settimeout(0.2)
    data = b""
    end = time.monotonic() + timeout
    while token not in data and time.monotonic() < end:
        try:
            data += sock.recv(4096)
        except socket.timeout:
            pass
    if token not in data:
        raise TimeoutError(data)
    return data


def call(sock, command, expected=b"OK\r\n"):
    global CHECKS
    sock.sendall(command.encode() + b"\r\n")
    data = read_until(sock, expected)
    if b":ERROR\r\n" in data:
        raise AssertionError((command, data))
    if command != "AT+GETSTATUS?":
        CHECKS += 1
    return data


def payload_call(sock, command, payload):
    global CHECKS
    sock.sendall(command.encode() + b"\r\n" + payload)
    data = read_until(sock)
    if b":ERROR\r\n" in data:
        raise AssertionError((command, data))
    CHECKS += 1
    return data


def echo_server(listener):
    while True:
        try:
            conn, _ = listener.accept()
        except OSError:
            return
        while True:
            try:
                data = conn.recv(4096)
            except ConnectionResetError:
                break
            if not data:
                break
            conn.sendall(data)
        conn.close()


def main():
    global CHECKS
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    a, b = socket.socketpair()
    env = dict(os.environ, AT_HOST_SSID="testnet", AT_HOST_PASS="")
    proc = subprocess.Popen([str(BL), str(b.fileno())], pass_fds=(b.fileno(),), env=env)
    thread = threading.Thread(target=echo_server, args=(listener,), daemon=True)
    thread.start()
    try:
        call(a, "AT+BEGINSTA=testnet,")
        end = time.monotonic() + 3
        while True:
            status = call(a, "AT+GETSTATUS?")
            if b"+GETSTATUS: 3\r\n" in status:
                break
            if time.monotonic() > end:
                raise AssertionError(status)
            time.sleep(0.05)
        begin = call(a, "AT+BEGINCLIENT")
        assert b"+BEGINCLIENT: 0\r\n" in begin, begin
        for bad in (
            "AT+CLIENTSTATE=2000", "AT+SSLCLIENTSTATE=0",
            "AT+IPCLIENT=-1", "AT+SSLIPCLIENT=9999",
            "AT+CLIENTCONNECTIP=0,not-an-ip,80",
            f"AT+CLIENTCONNECT=0,127.0.0.1,{port},",
        ):
            a.sendall(bad.encode() + b"\r\n")
            data = read_until(a, b"ERROR\r\n")
            assert b"OK\r\n" not in data, (bad, data)
            CHECKS += 1
        call(a, f"AT+CLIENTCONNECTIP=0,127.0.0.1,{port}")
        state = call(a, "AT+CLIENTSTATE=0")
        assert f"127.0.0.1,{port},".encode() in state, state
        local = call(a, "AT+IPCLIENT=0")
        assert b"+IPCLIENT: 127.0.0.1\r\n" in local, local
        call(a, "AT+CLIENTSTATUS=0")
        payload_call(a, "AT+CLIENTSEND=0,2", b"pk")
        peek = call(a, "AT+PEEK=0")
        assert b"+PEEK: 112\r\n" in peek, peek
        call(a, "AT+CLIENTRECEIVE=0,2")
        call(a, "AT+CLIENTCLOSE=0")
        for suffix in ("", ",1500", ",0"):
            call(a, "AT+BEGINCLIENT")
            call(a, f"AT+CLIENTCONNECT=0,127.0.0.1,{port}" + suffix)
            call(a, "AT+CLIENTCLOSE=0")

        ssl = call(a, "AT+SSLBEGINCLIENT")
        assert b"+SSLBEGINCLIENT: 2000\r\n" in ssl, ssl
        # The fake TLS port checks this exact test CA; this is not a credential.
        ca = (b"-----BEGIN CERTIFICATE-----\n"
              b"MIIBbjCCARSgAwIBAgIUPWXV8T0fBmdE0eKUp621QrZsl1AwCgYIKoZIzj0EAwIw\n"
              b"HTEbMBkGA1UEAwwSYXJkdWluby1ibCB0ZXN0IENBMB4XDTI2MTAyNDA5MTcyM1oX\n"
              b"DTM2MTAyMTA5MTcyM1owHTEbMBkGA1UEAwwSYXJkdWluby1ibCB0ZXN0IENBMFkw\n"
              b"EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYUH5p3Y4wObRzSWgxo7puKc5lFSQ+ATt\n"
              b"VuhNDColXMxc3bMbcIOqWzjuuqmU/BsM2Hkxzg8NbRXWag030HQJ/qMyMDAwHQYD\n"
              b"VR0OBBYEFPz7hy3GviF2JKYO5HoaH+Dwf49tMA8GA1UdEwEB/wQFMAMBAf8wCgYI\n"
              b"KoZIzj0EAwIDSAAwRQIgLjU9e7qNScnRl93FNewDLeNt6l7i9NV0pjQfrfojXtEC\n"
              b"IQCsjHa00XSN0CA39iEokf62PtKPcgb3ok+poS9dD9Pm+g==\n"
              b"-----END CERTIFICATE-----\n")
        payload_call(a, f"AT+SETCAROOT=2000,{len(ca)}", ca)
        a.sendall(f"AT+SSLCLIENTCONNECT=2000,127.0.0.1,{port},\r\n".encode())
        assert b"ERROR\r\n" in read_until(a, b"ERROR\r\n")
        CHECKS += 1
        call(a, f"AT+SSLCLIENTCONNECTIP=2000,127.0.0.1,{port}")
        sstate = call(a, "AT+SSLCLIENTSTATE=2000")
        assert f"127.0.0.1,{port},".encode() in sstate, sstate
        sip = call(a, "AT+SSLIPCLIENT=2000")
        assert b"+SSLIPCLIENT: 127.0.0.1\r\n" in sip, sip
        call(a, "AT+SSLCLIENTSTATUS=2000")
        payload_call(a, "AT+SSLCLIENTSEND=2000,2", b"pk")
        speak = call(a, "AT+SSLPEEK=2000")
        assert b"+SSLPEEK: 112\r\n" in speak, speak
        call(a, "AT+SSLCLIENTCLOSE=2000")
        print(f"ARDUINO-51 software/fake contract checks: {CHECKS} PASS")
    finally:
        a.close()
        b.close()
        listener.close()
        proc.terminate()
        proc.wait(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path)
    BL = parser.parse_args().server.resolve()
    main()
