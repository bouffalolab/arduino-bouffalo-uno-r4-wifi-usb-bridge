#!/usr/bin/env python3
"""Software/fake time and hostname protocol checks. No SNTP/DHCP/RF acceptance.

Uses real AT handlers + platform WiFiClass; fake wifi_port replaces RF/netifs.
Run run.py to build bl_host first.
"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile


def run(server, root, clock):
    a, b = socket.socketpair()
    a.settimeout(3)
    proc = subprocess.Popen([server, str(b.fileno())], pass_fds=(b.fileno(),),
                            env=dict(os.environ, AT_HOST_FS_ROOT=root, AT_HOST_TIME=clock))
    b.close()
    count = 0
    def check(command, value='', error=False):
        nonlocal count
        name = command[2:].split('=')[0].rstrip('?')
        expected = (name+':ERROR\r\n' if error else name+': '+value+'\r\nOK\r\n').encode()
        a.sendall(command.encode()+b'\r\n')
        got = b''
        while len(got) < len(expected):
            part = a.recv(len(expected)-len(got))
            if not part: break
            got += part
        assert got == expected, (command, got, expected)
        count += 1
        print('PASS', command, flush=True)
    try:
        check('AT+GETTIME=', clock)
        for cmd in ('AT+GETTIME', 'AT+GETTIME?', 'AT+GETTIME=x'):
            check(cmd, error=True)
        check('AT+HOSTNAME?', 'bl616cl-AABBCC')
        for name in ('a', 'A45-node', 'n'*31):
            check('AT+HOSTNAME='+name)
            check('AT+HOSTNAME?', name)
        for bad in ('', '-bad', 'bad-', 'has space', 'has.dot', 'has,comma', 'under_score', 'n'*32, '中文'):
            check('AT+HOSTNAME='+bad, error=True)
            check('AT+HOSTNAME?', 'n'*31)
        check('AT+HOSTNAME', error=True)
        check('AT+WIFIMODE?', '0')
        return count
    finally:
        a.close(); proc.terminate(); proc.wait(timeout=3)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('server')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='at-time-fake-') as root:
        count = sum(run(str(Path(args.server).resolve()), root, clock) for clock in ('0', '4102444800'))
    print(f'{count} software/fake checks passed; hardware acceptance: 0')


if __name__ == '__main__':
    main()
