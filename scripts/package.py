#!/usr/bin/env python3
"""Package a tested Linux x86-64 host, protocol schemas, and source provenance."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/bin/isolate-host')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    parser.add_argument('--dependency-root', type=Path,
                        default=ROOT/'build/workerd/bazel-workerd/external')
    args = parser.parse_args()
    pin = json.loads((ROOT / 'upstream.json').read_text())
    expected = f"isolate-host {pin['version']} protocol=1 source-format=1"
    actual = subprocess.check_output([str(args.binary.resolve()), '--version'], text=True).strip()
    if actual != expected:
        raise SystemExit(f'Wrong binary version: {actual!r}, expected {expected!r}')
    elf = subprocess.check_output(['readelf', '-h', str(args.binary)], text=True)
    if 'Advanced Micro Devices X86-64' not in elf:
        raise SystemExit('Only Linux x86-64 release artifacts are currently supported')
    symbols = subprocess.check_output(['readelf', '--version-info', str(args.binary)], text=True)
    glibc = sorted(set(re.findall(r'GLIBC_(\d+(?:\.\d+)+)', symbols)),
                   key=lambda s: tuple(map(int, s.split('.'))))
    dynamic = subprocess.check_output(['readelf', '-d', str(args.binary)], text=True)
    libraries = re.findall(r'Shared library: \[([^\]]+)\]', dynamic)
    unexpected = set(libraries) - {'libc.so.6', 'libm.so.6', 'ld-linux-x86-64.so.2',
                                  'libpthread.so.0', 'libdl.so.2', 'librt.so.1'}
    if unexpected:
        raise SystemExit(f'Unexpected runtime libraries: {sorted(unexpected)}')
    manifest = dict(pin, platform='linux-x86_64', minimumGlibc=glibc[-1] if glibc else None,
                    dynamicLibraries=libraries,
                    patchSha256=hashlib.sha256((ROOT/'patches/workerd.patch').read_bytes()).hexdigest(),
                    binarySha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                    sourceSha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in [ROOT/'src/isolate-host.c++',
                                            *sorted((ROOT/'protocol').glob('*.capnp'))]})
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / f"isolate-host-{pin['version']}-linux-x86_64.tar.xz"
    files = {'bin/isolate-host': args.binary, 'LICENSE': ROOT/'LICENSE',
             'README.md': ROOT/'README.md', 'NOTICE': ROOT/'NOTICE',
             'docs/validation.md': ROOT/'docs/validation.md',
             'upstream.json': ROOT/'upstream.json'}
    files.update({f'protocol/{p.name}': p for p in (ROOT/'protocol').glob('*.capnp')})
    # Preserve upstream notices for dependencies incorporated into the executable.
    if not args.dependency_root.is_dir():
        raise SystemExit('Dependency sources are required to collect release license notices')
    files['licenses/workerd-LICENSE'] = ROOT/'build/workerd/LICENSE'
    for repo in sorted(args.dependency_root.iterdir()):
        if not repo.is_dir():
            continue
        for path in repo.rglob('*'):
            name = path.name.lower()
            if (name in ('license', 'licence', 'notice', 'copying', 'copyright') or
                    name.startswith(('license-', 'license.', 'licence-', 'licence.', 'notice.', 'copying.'))):
                if path.is_file() and path.stat().st_size < 2 * 1024 * 1024:
                    files[f'licenses/{repo.name}/{path.relative_to(repo)}'] = path
    with tarfile.open(archive, 'w:xz') as tar:
        for name, path in sorted(files.items()):
            data = path.read_bytes()
            info = tarfile.TarInfo(name)
            info.mode = 0o755 if name.startswith('bin/') else 0o644
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        data = (json.dumps(manifest, indent=2) + '\n').encode()
        info = tarfile.TarInfo('manifest.json'); info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
    (args.output/'SHA256SUMS').write_text(
        hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(archive)


if __name__ == '__main__':
    main()
