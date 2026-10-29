#!/usr/bin/env python3
"""Software/fake UDP multicast AT contract checks; hardware acceptance: 0.

Real AT handlers and WiFiUDP, fake RF/netifs/IGMP and POSIX unicast sockets.
Group delivery and membership removal require separate real-board evidence.
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
    a, b = socket.socketpair()
    a.settimeout(3)
    proc = subprocess.Popen([server, str(b.fileno())], pass_fds=(b.fileno(),),
                            env=dict(os.environ, AT_HOST_FS_ROOT=root))
    b.close()
    count = 0

    def check(command, value='', error=False, payload=b''):
        nonlocal count
        name = command[2:].split('=')[0].rstrip('?')
        expected = (name+':ERROR\r\n' if error else name+': '+str(value)+'\r\nOK\r\n').encode()
        a.sendall(command.encode()+b'\r\n'+payload)
        got = b''
        while len(got) < len(expected):
            part = a.recv(len(expected)-len(got))
            if not part:
                break
            got += part
            if got.endswith(b':ERROR\r\n'):
                break
        assert got == expected, (command, got, expected)
        count += 1
        print('PASS', command, flush=True)

    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind(('127.0.0.1', 0))
    peer.settimeout(3)
    port = peer.getsockname()[1]
    try:
        check('AT+UDPBEGINMULTI=45150,239.50.0.1', error=True)
        check('AT+BEGINSTA=testnet')
        time.sleep(.35)
        check('AT+GETSTATUS?', 3)
        check('AT+UDPBEGINMULTI=45150,239.50.0.1', 0)
        check('AT+BEGINPACKETMULTI=0')
        check('AT+UDPBEGINPACKET=0')
        check('AT+UDPSTOP=0')
        for args in ('', '45150', '45150,', '0,239.50.0.1', '-1,239.50.0.1',
                     '65536,239.50.0.1', 'port,239.50.0.1', '45150,0.0.0.0',
                     '45150,223.255.255.255', '45150,240.0.0.0',
                     '45150,127.0.0.1', '45150,224.0.0.1', '45150,239.1.2.999', '45150,group',
                     '45150,239.50.0.1,extra'):
            check('AT+UDPBEGINMULTI='+args, error=True)
        check('AT+UDPBEGINMULTI', error=True)
        check('AT+UDPBEGINMULTI?', error=True)
        for command in ('BEGINPACKETMULTI', 'UDPBEGINPACKET'):
            for arg in ('', '-1', '4', '0', '1,2', 'x'):
                check('AT+'+command+'='+arg, error=True)
            check('AT+'+command, error=True)
            check('AT+'+command+'?', error=True)
        for i in range(4):
            check(f'AT+UDPBEGINMULTI=45150,239.50.0.{i+1}', i)
        check('AT+UDPBEGINMULTI=45151,239.50.0.5', error=True)
        check('AT+UDPSTOP=1')
        check('AT+UDPBEGINMULTI=45151,239.50.0.5', 1)
        for i in range(4):
            check(f'AT+UDPSTOP={i}')
        for _ in range(12):
            check('AT+UDPBEGINMULTI=45150,239.50.0.1', 0)
            check('AT+BEGINPACKETMULTI=0')
            check('AT+UDPSTOP=0')
        check('AT+UDPBEGIN=0', 0)
        check('AT+BEGINPACKETMULTI=0', error=True)
        check('AT+UDPBEGINPACKET=0', error=True)
        check('AT+UDPBEGINPACKETIP=0,%d,127.0.0.1' % port)
        check('AT+UDPBEGINPACKET=0')
        a.sendall(b'AT+UDPWRITE=0,3\r\nabc')
        assert a.recv(4) == b'OK\r\n'
        count += 1
        check('AT+UDPENDPACKET=0')
        data, address = peer.recvfrom(32)
        assert data == b'abc'
        count += 1
        peer.sendto(b'input', address)
        time.sleep(.05)
        check('AT+UDPPARSE=0', 5)
        check('AT+UDPREMOTEPORT=0', port)
        check('AT+UDPBEGINPACKET=0')
        a.sendall(b'AT+UDPWRITE=0,4\r\nnext')
        assert a.recv(4) == b'OK\r\n'
        count += 1
        check('AT+UDPENDPACKET=0')
        assert peer.recvfrom(32)[0] == b'next'
        count += 1
        check('AT+UDPSTOP=0')
        check('AT+UDPBEGIN=0', 0)
        check('AT+UDPBEGINPACKET=0', error=True)
        check('AT+UDPWRITE=4,4', error=True, payload=b'AT\r\n')
        check('AT+WIFIMODE?', 1)
        check('AT+UDPSTOP=0')
        return count
    finally:
        peer.close()
        a.close()
        proc.terminate()
        proc.wait(timeout=3)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('server')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='at-multicast-fake-') as root:
        count = run(str(Path(args.server).resolve()), root)
    print(f'{count} software/fake checks passed; hardware acceptance: 0')
