#!/usr/bin/env python3
"""Linux HID contract checks on a running bridge. No target memory is changed.

Usage: python3 tests/dap_hid_check.py /dev/hidrawN
Requires read/write permission for this board's HID node.
"""
import argparse
import fcntl
import os
import select
from pathlib import Path


def main():
    """Parse options and run the dap hid check validation or packaging command."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('device')
    args = parser.parse_args()
    identity = Path('/sys/class/hidraw') / Path(args.device).name / 'device/uevent'
    assert 'HID_ID=0003:0000349B:00000011' in identity.read_text(), 'wrong HID device'
    fd = os.open(args.device, os.O_RDWR | os.O_NONBLOCK)
    checks = 0

    def check(data, expected):
        """Validate the selected artifact or fixture and raise an error on a failed invariant."""
        nonlocal checks
        os.write(fd, b'\0' + bytes(data).ljust(64, b'\0'))
        assert select.select([fd], [], [], 3)[0], 'HID response timeout'
        response = os.read(fd, 64)
        assert len(response) == 64 and response.startswith(bytes(expected)), response.hex()
        checks += 1
        print(f'PASS {bytes(data).hex()} -> {response[:len(expected)].hex()}', flush=True)

    try:
        check([0, 0xff], [0, 2, 64, 0])  # 64-byte transport
        check([0, 0xfe], [0, 1, 1])      # one outstanding packet
        check([0, 0xf0], [0, 1, 1])      # SWD only, no JTAG or SWO claims
        check([0x80], [0x80, 0xff])       # unimplemented vendor command
        check([0xff], [0xff])
        check([2, 2], [2, 0])            # JTAG is not supported
        check([0x11, 0, 0, 0, 0], [0x11, 0xff])
        check([0x11, 0, 0, 0, 0x80], [0x11, 0xff])
        check([0x11, 0xa0, 0x86, 1, 0], [0x11, 0])  # 100 kHz
        for _ in range(100):
            check([0, 0xff], [0, 2, 64, 0])
        feature = bytearray(65)  # report ID 0, then up to 64 bytes
        fcntl.ioctl(fd, 0xc0414807, feature, True)  # HIDIOCGFEATURE(65)
        assert feature[1:4] == bytes([0, 1, 0]), feature.hex()
        checks += 1
        print(f'{checks} checks passed; feature version 0.1.0')
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
