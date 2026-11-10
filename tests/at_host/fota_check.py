#!/usr/bin/env python3
"""AT framing tests with a fake update backend; not a Flash or RA4 test."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile

from file_check import exchange


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('server')
    server = str(Path(parser.parse_args().server).resolve())
    a, b = socket.socketpair()
    a.settimeout(5)
    with tempfile.TemporaryDirectory(prefix='at-fota-fake-') as directory:
        proc = subprocess.Popen([server, str(b.fileno())], pass_fds=(b.fileno(),),
                                env=dict(os.environ, AT_HOST_FS_ROOT=directory),
                                stderr=subprocess.PIPE)
        b.close()
        count = 0
        def check(name, request, response, fragments=()):
            nonlocal count
            exchange(a, request, response, fragments)
            count += 1
            print('PASS', name, flush=True)
        def result(command):
            return b'+' + command + b': 0\r\nOK\r\n'
        header = bytes(range(256)) * 2
        chunk = bytes(range(256))
        try:
            check('invalid header length drains bytes', b'AT+BLFOTABEGIN=3\r\nxyzAT\r\n',
                  b'+BLFOTABEGIN:ERROR\r\nOK\r\n')
            check('begin fragmented binary header', b'AT+BLFOTABEGIN=512\r\n',
                  result(b'BLFOTABEGIN'), (header[:17], header[17:]))
            check('first data', b'AT+BLFOTADATA=0,256\r\n' + chunk, result(b'BLFOTADATA'))
            check('duplicate offset aborts session', b'AT+BLFOTADATA=0,256\r\n' + chunk,
                  b'+BLFOTADATA:ERROR\r\n')
            check('finish after failure rejected', b'AT+BLFOTAFINISH\r\n', b'+BLFOTAFINISH:ERROR\r\n')
            check('new session', b'AT+BLFOTABEGIN=512\r\n' + header, result(b'BLFOTABEGIN'))
            check('oversized frame is drained', b'AT+BLFOTADATA=0,1025\r\n' + bytes(1025) + b'AT\r\n',
                  b'+BLFOTADATA:ERROR\r\nOK\r\n')
            check('restart after abort', b'AT+BLFOTABEGIN=512\r\n' + header, result(b'BLFOTABEGIN'))
            check('whole image frame', b'AT+BLFOTADATA=0,512\r\n' + header, result(b'BLFOTADATA'))
            check('verify', b'AT+BLFOTAFINISH\r\n', result(b'BLFOTAFINISH'))
            check('commit replies before reset', b'AT+BLFOTACOMMIT\r\n', result(b'BLFOTACOMMIT'))
            proc.wait(timeout=3)
            assert proc.returncode == 0 and b'RESETTOBOOTLOADER' in proc.stderr.read()
            print(f'{count} software/fake checks passed; hardware acceptance: 0')
        finally:
            a.close()
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=3)


if __name__ == '__main__':
    main()
