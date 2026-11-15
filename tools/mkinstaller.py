#!/usr/bin/env python3
# Makes PiArcBoot's installer: the stub (tools/stub.c, built for the system the installer is for)
# with the PiArcBoot folder packed behind it in the layout stub.c describes, and the CRC-32 that
# lets the stub tell a damaged download.
#   python3 tools/mkinstaller.py <stub> <PiArcBoot folder> <installer>
import os
import struct
import sys
import zlib

stub, folder, out = sys.argv[1:4]
base = os.path.dirname(os.path.abspath(folder))
payload = bytearray()
for root, dirs, files in os.walk(folder):
    dirs.sort()
    for name in sorted(files):
        path = os.path.join(root, name)
        rel = os.path.relpath(path, base).replace(os.sep, '/').encode()
        with open(path, 'rb') as f:
            data = f.read()
        mode = os.stat(path).st_mode & 0o777
        payload += struct.pack('<HIQ', len(rel), mode, len(data)) + rel + data
with open(out, 'wb') as o:
    with open(stub, 'rb') as f:
        o.write(f.read())
    start = o.tell()
    o.write(payload)
    o.write(struct.pack('<QII', start, zlib.crc32(payload) & 0xFFFFFFFF, 0) + b'PABSETUP')
os.chmod(out, 0o755)
