#!/usr/bin/env python3
"""Build the standalone host from a pinned workerd checkout and local overlay."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
PIN = json.loads((ROOT / 'upstream.json').read_text())


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def prepare(source, build):
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != PIN['workerd']:
        raise SystemExit(f"Expected workerd {PIN['workerd']}, got {commit}")
    # Only committed contents enter the build, never local modifications or generated files.
    inputs = [ROOT / 'upstream.json', Path(__file__), ROOT / 'patches/workerd.patch', ROOT / 'src/isolate-host.c++',
              ROOT / 'tests/bundle-test.c++', *sorted((ROOT / 'protocol').glob('*.capnp'))]
    fingerprint = hashlib.sha256(commit.encode() + b''.join(p.read_bytes() for p in inputs)).hexdigest()
    work = build / 'workerd'
    stamp = work / '.isolate-host-source'
    if stamp.exists() and stamp.read_text() == fingerprint:
        return work
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    archive = build / 'workerd-source.tar'
    with archive.open('wb') as out:
        run('git', '-C', source, 'archive', commit, stdout=out)
    with tarfile.open(archive) as tar:
        tar.extractall(work, filter='data')
    archive.unlink()
    run('patch', '--batch', '--fuzz=0', '-p1', '-i', ROOT / 'patches/workerd.patch', cwd=work)
    server = work / 'src/workerd/server'
    (server / 'isolate-host-version.h').write_text(
        '#define ISOLATE_HOST_VERSION ' + json.dumps(PIN['version']) + '\n'
        '#define ISOLATE_HOST_WORKERD_COMMIT ' + json.dumps(PIN['workerd']) + '\n')
    shutil.copy2(ROOT / 'src/isolate-host.c++', server)
    build_file = server / 'BUILD.bazel'
    build_file.write_text(build_file.read_text().replace(
        'srcs = ["isolate-host.c++"]',
        'srcs = ["isolate-host.c++", "isolate-host-version.h"]'))
    (server / 'isolate-host-bundle-test.c++').write_text(
        '#define main isolateHostProgramMain\n' + (ROOT/'src/isolate-host.c++').read_text() +
        '\n#undef main\n' + (ROOT/'tests/bundle-test.c++').read_text())
    contents = build_file.read_text()
    start = contents.index('wd_cc_binary(\n    name = "isolate-host",')
    end = contents.index('\n)\n', start) + 3
    test_target = contents[start:end].replace('"isolate-host"', '"isolate-host-bundle-test"')
    test_target = test_target.replace('"isolate-host.c++"', '"isolate-host-bundle-test.c++"')
    build_file.write_text(contents + '\n' + test_target)
    for schema in (ROOT / 'protocol').glob('*.capnp'):
        shutil.copy2(schema, server)
    stamp.write_text(fingerprint)
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='existing pinned upstream workerd checkout')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--bazel', type=Path)
    parser.add_argument('--output-base', type=Path, help='optional Bazel output base')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--test', action='store_true')
    parser.add_argument('--client-capnp-source', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--bazel-option', action='append', default=[])
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source = args.source or build / 'upstream'
    if not source.exists():
        run('git', 'init', source)
        run('git', '-C', source, 'fetch', '--depth=1', 'https://github.com/cloudflare/workerd.git', PIN['workerd'])
        run('git', '-C', source, 'checkout', '--detach', 'FETCH_HEAD')
    work = prepare(source.resolve(), build)
    if args.prepare_only:
        return
    bazel = args.bazel or build / f"bazel-{PIN['bazel']}"
    if not bazel.exists():
        url = f"https://github.com/bazelbuild/bazel/releases/download/{PIN['bazel']}/bazel-{PIN['bazel']}-linux-x86_64"
        tmp = bazel.with_suffix('.download')
        urllib.request.urlretrieve(url, tmp)
        if hashlib.sha256(tmp.read_bytes()).hexdigest() != PIN['bazelLinuxX86_64Sha256']:
            tmp.unlink()
            raise SystemExit('Bazel checksum mismatch')
        tmp.chmod(0o755)
        tmp.replace(bazel)
    command = [bazel.resolve(), '--batch', f'--output_user_root={build / "bazel"}']
    if args.output_base:
        command.append(f'--output_base={args.output_base.resolve()}')
    command += ['build',
               '--config=release', f'--jobs={args.jobs}',
               f'--repository_cache={build / "repository-cache"}',
               f'--repo_contents_cache={build / "repo-contents-cache"}',
               f'--disk_cache={build / "action-cache"}', *args.bazel_option]
    if os.environ.get('CC'):
        command.append(f'--repo_env=CC={os.environ["CC"]}')
    targets = ['//src/workerd/server:isolate-host']
    if args.test:
        targets.append('//src/workerd/server:isolate-host-bundle-test')
    run(*command, *targets, cwd=work)
    out = build / 'bin'
    out.mkdir(exist_ok=True)
    for name in ('isolate-host',):
        shutil.copy2(work / 'bazel-bin/src/workerd/server' / name, out / name)
    if args.test:
        run(work / 'bazel-bin/src/workerd/server/isolate-host-bundle-test')
        run('cmake', '-S', ROOT / 'tests', '-B', build / 'tests',
            '-DCMAKE_BUILD_TYPE=Release',
            *([f'-DISOLATE_TEST_CAPNP_SOURCE={args.client_capnp_source.resolve()}']
              if args.client_capnp_source else []))
        run('cmake', '--build', build / 'tests', '-j', args.jobs)
        run('bash', ROOT / 'tests/control.sh', out / 'isolate-host',
            build / 'tests/isolate-host-control-client', build / 'control.sock')
        run(sys.executable, ROOT / 'tests/inherited-fd.py', out / 'isolate-host',
            build / 'tests/isolate-host-control-client')


if __name__ == '__main__':
    main()
