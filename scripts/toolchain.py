#!/usr/bin/env python3
"""Install the pinned Chromium Clang used by the extracted host build."""
import base64
import hashlib
from pathlib import Path
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
REVISION = '3074f6739c4cba4154d7086253cec5f5bcdabb20'
UPDATER_SHA256 = 'b2dcb53c0e4c9b79ec7c614f02289c4abc6a60f826cdfdf88837932cafe34b50'


def main():
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    url = f'https://chromium.googlesource.com/chromium/src/tools/clang/+/{REVISION}/scripts/update.py?format=TEXT'
    with urllib.request.urlopen(url) as response:
        script = base64.b64decode(response.read())
    if hashlib.sha256(script).hexdigest() != UPDATER_SHA256:
        raise SystemExit('Clang updater checksum mismatch')
    updater = build / 'clang-update.py'
    updater.write_bytes(script)
    subprocess.run([sys.executable, str(updater), '--output-dir', str(build / 'llvm')], check=True)


if __name__ == '__main__':
    main()
