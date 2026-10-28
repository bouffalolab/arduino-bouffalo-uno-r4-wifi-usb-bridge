#!/usr/bin/env python3
"""Software/fake FS tests of the actual AT server; not hardware acceptance.

Build bl_host with run.py first. POSIX storage and fault injection replace Flash
and littlefs. This checks wire framing, chunk boundaries and error handling.
"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def exchange(sock, request, expected, fragments=()):
    for part in (request,) + tuple(fragments):
        sock.sendall(part)
        if fragments:
            time.sleep(0.015)
    result = b""
    while len(result) < len(expected):
        data = sock.recv(len(expected) - len(result))
        if not data:
            break
        result += data
    assert result == expected, (request[:100], result, expected[:100])


def run(server, directory, fault=None):
    a, b = socket.socketpair()
    a.settimeout(3)
    env = dict(os.environ, AT_HOST_FS_ROOT=directory)
    env.update(fault or {})
    proc = subprocess.Popen([server, str(b.fileno())], pass_fds=(b.fileno(),), env=env)
    b.close()
    count = 0
    def check(name, request, response, fragments=()):
        nonlocal count
        exchange(a, request, response, fragments)
        count += 1
        print("PASS", name, flush=True)
    ok, error = b"OK\r\n", b"+FS:ERROR\r\n"
    try:
        if fault and 'AT_HOST_FS_UNMOUNTABLE' in fault:
            for flag in (0, 1):
                check('mount failure never formats', f'AT+MOUNTFS={flag}\r\n'.encode(), b'+MOUNTFS:ERROR\r\n')
            return count
        check('unmounted write drains AT-looking binary', b'AT+FS=0,1,/a,4\r\nAT\r\nAT\r\n', error + ok)
        check('explicit mount', b'AT+MOUNTFS=0\r\n', b'+MOUNTFS: FS Mounted\r\n\r\nOK\r\n')
        data = bytes(range(255))
        if fault:
            check('write/close failure drains next command', b'AT+FS=0,1,/a,255\r\n'+data+b'AT\r\n', error+ok)
            return count
        check('segmented binary write', b'AT+FS=0,1,/a,255\r\n', ok, (data[:11], data[11:180], data[180:]))
        check('binary read exact framing', b'AT+FS=0,2,/a,0,1023\r\n', b'+FS: 255|'+data+b'\r\nOK\r\n')
        check('append binary', b'AT+FS=0,3,/a,5\r\n\0\r\n|,', ok)
        check('offset read', b'AT+FS=0,2,/a,254,100\r\n', b'+FS: 6|'+data[254:]+b'\0\r\n|,\r\nOK\r\n')
        check('EOF terminates official host read loop', b'AT+FS=0,2,/a,260,1023\r\n', error)
        check('empty file real create', b'AT+FS=0,1,/empty,0\r\n', ok)
        assert (Path(directory)/'empty').stat().st_size == 0
        check('empty read EOF', b'AT+FS=0,2,/empty,0,1023\r\n', error)
        for bad in (b'0,1,/../escape,4', b'7,1,/bad,4', b'0,1,/bad,4,extra'):
            check('invalid destination/header still drains', b'AT+FS='+bad+b'\r\nAT\r\nAT\r\n', error+ok)
        check('unsupported 256-byte frame drains fully', b'AT+FS=0,1,/bad,256\r\n'+bytes(range(256))+b'AT\r\n', error+ok)
        for bad in (b'0,2,/missing,0,2', b'0,2,/a,-1,2', b'0,2,/a,0,1024', b'0,2,/a,foo,2', b'0,4,/a', b'0,1,/a,-1', b'0,1,/a'):
            check('invalid/read missing error and synchronization', b'AT+FS='+bad+b'\r\nAT\r\n', error+ok)
        check('partial payload timeout', b'AT+FS=0,1,/timeout,5\r\nx', error)
        check('command after sender abandons timed-out frame', b'AT\r\n', ok)
        check('delete', b'AT+FS=0,0,/a\r\n', ok)
        check('delete missing', b'AT+FS=0,0,/a\r\n', error)
        return count
    finally:
        a.close()
        proc.terminate()
        proc.wait(timeout=3)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('server')
    args = ap.parse_args()
    total = 0
    for fault in (None, {'AT_HOST_FS_LIMIT': '100'}, {'AT_HOST_FS_CLOSE_FAIL': '1'}, {'AT_HOST_FS_UNMOUNTABLE': '1'}):
        with tempfile.TemporaryDirectory(prefix='at-fs-fake-') as d:
            total += run(str(Path(args.server).resolve()), d, fault)
    print(f'{total} software/fake checks passed; hardware acceptance: 0')


if __name__ == '__main__':
    main()
