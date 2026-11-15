#!/usr/bin/env python3
# Makes PiArcBoot's installer: the stub (tools/stub.c, built for the system the installer is for)
# with the PiArcBoot folder packed behind it in the layout stub.c describes.
#   python3 tools/mkinstaller.py <stub> <PiArcBoot folder> <installer>
import os
import struct
import sys

stub, folder, out = sys.argv[1:4]
base = os.path.dirname(os.path.abspath(folder))
with open(out, 'wb') as o:
    with open(stub, 'rb') as f:
        o.write(f.read())
    start = o.tell()
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, base).replace(os.sep, '/').encode()
            with open(path, 'rb') as f:
                data = f.read()
            mode = os.stat(path).st_mode & 0o777
            o.write(struct.pack('<HIQ', len(rel), mode, len(data)) + rel + data)
    o.write(struct.pack('<Q', start) + b'PABSETUP')
os.chmod(out, 0o755)
