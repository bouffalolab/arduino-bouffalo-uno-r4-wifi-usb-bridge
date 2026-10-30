#!/usr/bin/env python3
"""Software/fake AT contract checks. No radio/IPv6/reconnect acceptance.

Uses real AT handlers + platform WiFiClass; fake wifi_port replaces RF/netifs.
Run run.py to build bl_host first.
"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def run(server, root):
    """Run the selected host-only test scenario with isolated fixture resources."""
    a, b = socket.socketpair()
    a.settimeout(3)
    proc = subprocess.Popen([server, str(b.fileno())], pass_fds=(b.fileno(),),
                            env=dict(os.environ, AT_HOST_FS_ROOT=root))
    b.close()
    count = 0
    def check(command, value='', error=False):
        """Host-only fixture: validate the selected artifact or fixture and raise an error on a failed invariant."""
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
        check('AT+WIFIMODE?', '0')
        check('AT+IPV6', error=True)
        check('AT+RECONNECT?', '1')
        for cmd in ('WIFIMODE','RECONNECT','AUTOCONNECT','DISCONNECT','DISCONNECTSOFTAP'):
            for bad in ('-1','4','x','1,2,3',''):
                check('AT+'+cmd+'='+bad, error=True)
        check('AT+WIFIMODE?', '0')
        for value in ('0','1'):
            check('AT+AUTOCONNECT='+value)
            check('AT+AUTOCONNECT?', '0')  # Pinned official placeholder.
            check('AT+RECONNECT='+value)
            check('AT+RECONNECT?', value)
        check('AT+WIFIMODE=1,1')
        check('AT+WIFIMODE?', '1')
        check('AT+BEGINSTA=testnet')
        time.sleep(.35)
        check('AT+GETSTATUS?', '3')
        check('AT+SOFTAPCONFIG=192.168.49.1,192.168.49.1,255.255.255.0')
        check('AT+IPSTA=0', '127.0.0.1')
        check('AT+WIFIMODE=3,0')
        check('AT+BEGINSOFTAP=hidden,,6,1,1', '1')
        check('AT+GETSTATUS?', '3')
        check('AT+IPSOFTAP', '192.168.49.1')
        check('AT+SETIP=127.0.0.9,127.0.0.1,255.255.255.0')
        check('AT+IPSTA=0', '127.0.0.9')
        check('AT+IPSOFTAP', '192.168.49.1')
        for bad in ('ap,,6,2,1','ap,,6,1,1,extra'):
            check('AT+BEGINSOFTAP='+bad, error=True)
        for bad in ('ap,short,6','ap,,14','ap,,6,0,0','ap,,6,0,6'):
            check('AT+BEGINSOFTAP='+bad, '0')
        check('AT+GETSOFTAPSSID?', 'hidden')
        check('AT+GETSTATUS?', '3')
        check('AT+IPV6')
        check('AT+IPV6?', 'fe80::1')  # Fake payload only, no ND/RF claim.
        check('AT+DISCONNECTSOFTAP')
        check('AT+WIFIMODE?', '3')
        check('AT+IPSOFTAP', '0.0.0.0')
        check('AT+IPSTA=0', '127.0.0.9')
        check('AT+BEGINSOFTAP=again', '1')
        check('AT+DISCONNECT=1')
        check('AT+WIFIMODE?', '2')
        check('AT+IPSTA=0', '0.0.0.0')
        check('AT+IPSOFTAP', '192.168.49.1')
        check('AT+DISCONNECTSOFTAP=1')
        check('AT+WIFIMODE?', '0')
        check('AT+IPSOFTAP', '0.0.0.0')
        check('AT+WIFIMODE=3')
        check('AT+BEGINSTA=testnet')
        time.sleep(.35)
        check('AT+GETSTATUS?', '3')
        check('AT+WIFIMODE=0')
        check('AT+IPSTA=0', '0.0.0.0')
        check('AT+WIFIMODE?', '0')
        return count
    finally:
        a.close(); proc.terminate(); proc.wait(timeout=3)


def main():
    """Parse options and run the net check validation or packaging command."""
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('server')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='at-net-fake-') as root:
        count = run(str(Path(args.server).resolve()), root)
    print(f'{count} software/fake checks passed; hardware acceptance: 0')


if __name__ == '__main__':
    main()
