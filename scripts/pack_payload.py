#!/usr/bin/env python3
"""Append the install payload to OMTMini-Setup.exe.

Layout is documented in installer/payload.h.
"""
import os
import struct
import sys

MAGIC = b'OMTMPKG1'


def main():
    if len(sys.argv) < 4:
        print('usage: pack_payload.py <setup.exe> <output.exe> <file> [file...]',
              file=sys.stderr)
        return 2

    setup_path, out_path = sys.argv[1], sys.argv[2]
    files = sys.argv[3:]

    with open(setup_path, 'rb') as f:
        stub = f.read()

    payload = bytearray()
    for path in files:
        name = os.path.basename(path).encode('utf-8')
        with open(path, 'rb') as f:
            data = f.read()
        payload += struct.pack('<H', len(name)) + name
        payload += struct.pack('<Q', len(data)) + data

    footer = MAGIC + struct.pack('<IQ', len(files), len(stub))

    with open(out_path, 'wb') as f:
        f.write(stub)
        f.write(payload)
        f.write(footer)

    total = len(stub) + len(payload) + len(footer)
    print('packed {} file(s), setup is {:.1f} MB'.format(len(files), total / 1048576.0))
    for path in files:
        print('  {:<24} {:>10,} bytes'.format(os.path.basename(path),
                                              os.path.getsize(path)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
