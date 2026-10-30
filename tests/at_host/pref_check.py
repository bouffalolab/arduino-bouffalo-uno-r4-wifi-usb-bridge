#!/usr/bin/env python3
"""Software/fake checks: actual Preferences and AT server, POSIX FS, no real littlefs/Flash."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def number(command, value):
    """Construct the expected numeric AT response used by the host test."""
    return f'+{command}: {value}\r\n\r\nOK\r\n'.encode()


class Probe:
    def __init__(self, server, directory, fault=None):
        """Initialize the isolated host-test fixture and its owned resources."""
        self.sock, child = socket.socketpair(); self.sock.settimeout(3)
        self.proc = subprocess.Popen([server, str(child.fileno())], pass_fds=(child.fileno(),),
            env=dict(os.environ, AT_HOST_FS_ROOT=directory, **(fault or {})))
        child.close(); self.count = 0

    def check(self, name, request, expected, fragments=()):
        """Host-only fixture: validate the selected artifact or fixture and raise an error on a failed invariant."""
        for part in (request,) + tuple(fragments):
            self.sock.sendall(part)
            if fragments: time.sleep(.015)
        response = b''
        try:
            while len(response) < len(expected):
                block = self.sock.recv(len(expected)-len(response))
                if not block: break
                response += block
        except socket.timeout:
            raise AssertionError((name, response, expected[:120])) from None
        assert response == expected, (name, request[:100], response[:120], expected[:120])
        self.count += 1; print('PASS', name, flush=True)

    def command(self, command, arguments, value):
        """Send one Preferences host-test command and verify its numeric response."""
        self.check(command+' '+arguments, f'AT+{command}{arguments}\r\n'.encode(), number(command,value))

    def close(self):
        """Close the host-test fixture and release its process and socket resources."""
        self.sock.close(); self.proc.terminate(); self.proc.wait(timeout=3)


def standard(server, directory):
    """Run the normal Preferences host-fixture checks."""
    p=Probe(server,directory)
    try:
        p.check('unopened stat',b'AT+PREFSTAT\r\n',b'+PREFSTAT:ERROR\r\n')
        p.command('PREFBEGIN','=test,0,',1)
        p.command('PREFCLEAR','',1)
        p.command('PREFSTAT','',64)
        values=['-128','255','-32768','65535','-2147483648','4294967295','-9223372036854775808','18446744073709551615']
        for t,v in enumerate(values):
            p.command('PREFPUT',f'=n{t},{t},{v}',1 << (t//2))
            p.command('PREFTYPE',f'=n{t}',t)
            p.command('PREFGET',f'=n{t},{t},0',v)
        payload=bytes((i*37)&255 for i in range(1023))
        p.check('segmented binary max value',b'AT+PREFPUT=blob,9,1023\r\n',number('PREFPUT',1023),
                (payload[:11],payload[11:600],payload[600:]))
        p.command('PREFLEN','=blob',1023)
        p.check('binary exact framing',b'AT+PREFGET=blob,9\r\n',b'+PREFGET: 1023|'+payload+b'\r\nOK\r\n')
        text='中文, |\r\n'.encode()
        p.check('string raw comma and CRLF',f'AT+PREFPUT=text,8,{len(text)}\r\n'.encode()+text,number('PREFPUT',len(text)))
        p.check('string exact',b'AT+PREFGET=text,8,fallback\r\n',f'+PREFGET: {len(text)}|'.encode()+text+b'\r\nOK\r\n')
        p.command('PREFPUT','=empty,8,0',0)
        p.command('PREFTYPE','=empty',8)
        p.check('empty string exists',b'AT+PREFGET=empty,8,fallback\r\n',b'+PREFGET: 0|\r\nOK\r\n')
        p.command('PREFGET','=absent,4,123',123)
        p.command('PREFGET','=text,0,42',42)
        p.check('missing string default',b'AT+PREFGET=absent,8,default\r\n',b'+PREFGET: 7|default\r\nOK\r\n')
        p.check('missing blob',b'AT+PREFGET=absent,9\r\n',b'+PREFGET: 0|\r\nOK\r\n')
        p.command('PREFTYPE','=absent',10)
        p.command('PREFLEN','=text',0)
        for args in ['=bad,0,128','=bad,1,-1','=bad,2,-32769','=bad,3,65536',
                     '=bad,4,2147483648','=bad,5,4294967296','=bad,6,9223372036854775808',
                     '=bad,6,-9223372036854775809','=bad,7,18446744073709551616','=bad,10,1','=bad,4,x']:
            p.check('invalid numeric '+args,('AT+PREFPUT'+args+'\r\nAT\r\n').encode(),b'+PREFPUT:ERROR\r\nOK\r\n')
        for header,data in [(b'bad,9,1024',bytes(1024)),(b'abcdefghijklmnop,9,4',b'AT\r\n'),
                            (b'bad,8,4',b'A\0\r\n'),(b'bad,9,4,extra',b'AT\r\n')]:
            p.check('invalid payload still consumes',b'AT+PREFPUT='+header+b'\r\n'+data+b'AT\r\n',b'+PREFPUT:ERROR\r\nOK\r\n')
        p.check('partial payload timeout',b'AT+PREFPUT=short,9,5\r\nx',b'+PREFPUT:ERROR\r\n')
        p.check('sync after abandoned frame',b'AT\r\n',b'OK\r\n')
        for args in ['=text,4','=text,9,extra','=text,11,0','=text,1,-1']:
            p.check('invalid get '+args,('AT+PREFGET'+args+'\r\n').encode(),b'+PREFGET:ERROR\r\n')
        # A real snapshot now exists. Raw access would expose or truncate key data.
        for alias in ['/.preferences/74657374.kv','//.preferences/74657374.kv','/./.preferences/74657374.kv']:
            p.check('reserved path read '+alias,('AT+FS=0,2,'+alias+',0,1\r\n').encode(),b'+FS:ERROR\r\n')
            p.check('reserved path write drains '+alias,('AT+FS=0,1,'+alias+',4\r\nAT\r\nAT\r\n').encode(),b'+FS:ERROR\r\nOK\r\n')
        p.command('PREFBEGIN','=test,1,',1)
        p.command('PREFPUT','=n0,0,7',0)
        p.command('PREFCLEAR','',0)
        p.command('PREFREMOVE','=n0',0)
        p.command('PREFGET','=n0,0,0',-128)
        p.command('PREFBEGIN','=other,0,lfs',1)
        p.command('PREFPUT','=n0,4,7',4)
        p.command('PREFCLEAR','',1)
        p.command('PREFBEGIN','=test,0,',1)
        p.command('PREFGET','=n0,0,0',-128)
        p.command('PREFREMOVE','=absent',0)
        p.command('PREFBEGIN','=missing,1,',0)
        p.command('PREFBEGIN','=test,0,nvs',0)
        p.command('PREFBEGIN','=slots,0,',1)
        for n in range(64): p.command('PREFPUT',f'=k{n},4,{n}',4)
        p.command('PREFSTAT','',0)
        p.command('PREFPUT','=overflow,4,1',0)
        p.command('PREFPUT','=k0,4,99',4)
        p.command('PREFGET','=k0,4,0',99)
        p.command('PREFREMOVE','=k63',1)
        p.command('PREFSTAT','',1)
        p.command('PREFCLEAR','',1)
        p.command('PREFSTAT','',64)
        p.check('end idempotent',b'AT+PREFEND\r\nAT+PREFEND\r\n',b'+PREFEND: 0\r\nOK\r\n'*2)
        return p.count
    finally: p.close()


def faults(server,directory):
    """Run Preferences host-fixture fault-injection checks."""
    total=0
    seed=Probe(server,directory)
    try:
        seed.command('PREFBEGIN','=safe,0,',1);seed.command('PREFPUT','=old,4,7',4);total+=seed.count
    finally: seed.close()
    for env in [{'AT_HOST_FS_LIMIT':'40'},{'AT_HOST_FS_CLOSE_FAIL':'1'},{'AT_HOST_FS_RENAME_FAIL':'1'}]:
        p=Probe(server,directory,env)
        try:
            p.command('PREFBEGIN','=safe,0,',1)
            p.check('failed replacement reports zero',b'AT+PREFPUT=new,9,100\r\n'+bytes(100)+b'AT\r\n',number('PREFPUT',0)+b'OK\r\n')
            p.command('PREFGET','=old,4,0',7)
            p.command('PREFTYPE','=new',10)
            total+=p.count
        finally:p.close()
    p=Probe(server,directory,{'AT_HOST_FS_UNMOUNTABLE':'1'})
    try:p.command('PREFBEGIN','=safe,0,',0);total+=p.count
    finally:p.close()
    path=Path(directory)/'.preferences'/('safe'.encode().hex()+'.kv')
    raw=path.read_bytes();path.write_bytes(raw[:4]+b'\x02\x00'+raw[6:])
    p=Probe(server,directory)
    try:p.command('PREFBEGIN','=safe,0,',0);assert path.read_bytes()[4:6]==b'\x02\x00';total+=p.count
    finally:p.close()
    return total


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('server');args=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='pref-at-fake-') as directory:
        total=standard(str(Path(args.server).resolve()),directory)
        total+=faults(str(Path(args.server).resolve()),directory)
    print(total,'software/fake checks passed; real littlefs/Flash hardware acceptance: 0')
