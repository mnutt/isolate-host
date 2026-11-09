from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import yaml

root = Path.cwd()
workflow = yaml.load((root/'.github/workflows/release.yml').read_text(), Loader=yaml.BaseLoader)
steps = workflow['jobs']['linux-x86_64']['steps']
by_name = {s.get('name'): s for s in steps}
for job in workflow['jobs'].values():
    for step in job['steps']:
        if 'run' in step:
            subprocess.run(['bash', '-n'], input=step['run'], text=True, check=True)
assert workflow['on']['push']['tags'] == ['v*']
assert workflow['jobs']['publish']['needs'] == 'linux-x86_64'
assert 'github.event_name' in workflow['jobs']['publish']['if']
compiler_key = by_name['Restore pinned compiler']['with']['key']
assert 'github.sha' not in compiler_key and 'toolchain.py' in compiler_key
restore = by_name['Restore Bazel caches']['with']
save = by_name['Save Bazel caches']
assert save['with']['path'] == restore['path']
assert save['if'].startswith('always()')
assert 'github.run_attempt' in restore['key']
assert 'steps.toolchain.outputs.hash' in restore['restore-keys']
assert 'build/repo-contents-cache' in restore['path']
assert 'build/repository-cache' not in restore['path']
downloads = by_name['Restore Bazel downloads']['with']
assert downloads['path'] == 'build/repository-cache'
assert 'toolchain' not in downloads['key'] + downloads['restore-keys']
assert by_name['Save Bazel downloads']['with']['path'] == downloads['path']
assert by_name['Save Bazel downloads']['if'].startswith('always()')
legacy = by_name['Restore legacy combined cache']
assert legacy['if'] == "steps.bazel-cache.outputs.cache-matched-key == ''"
assert 'steps.toolchain.outputs.legacy-hash' in legacy['with']['restore-keys']
assert legacy['with']['path'].splitlines() == [
    'build/repository-cache', 'build/repo-contents-cache', 'build/action-cache']

with tempfile.TemporaryDirectory(dir=root/'build') as temp:
    temp = Path(temp)
    (temp/'build/llvm/bin').mkdir(parents=True)
    (temp/'scripts').mkdir()
    (temp/'fake-bin').mkdir()
    (temp/'build/llvm/bin/clang').write_bytes(b'compiler-v1')
    shutil.copy(root/'upstream.json', temp/'upstream.json')
    shutil.copy(root/'scripts/toolchain.py', temp/'scripts/toolchain.py')
    dpkg = temp/'fake-bin/dpkg-query'
    dpkg.write_text('#!/bin/sh\nprintf "%s\\n" "$TEST_PACKAGES"\n')
    dpkg.chmod(0o755)
    env = dict(os.environ, PATH=str(temp/'fake-bin')+':'+os.environ['PATH'],
               ImageOS='ubuntu24', ImageVersion='20261029',
               GITHUB_OUTPUT=str(temp/'outputs'), TEST_PACKAGES='libc6=1\nlibc++-19-dev=2')
    def fingerprint():
        Path(env['GITHUB_OUTPUT']).write_text('')
        subprocess.run(['bash','-e','-o','pipefail','-c',by_name['Fingerprint Bazel toolchain']['run']],
                       cwd=temp, env=env, check=True)
        return dict(line.split('=', 1) for line in Path(env['GITHUB_OUTPUT']).read_text().splitlines())['hash']
    base = fingerprint()
    old_inputs = json.loads((temp/'build/toolchain-fingerprint.json').read_text())
    old_inputs.update(imageVersion=env['ImageVersion'],
                      packages=sorted(env['TEST_PACKAGES'].splitlines()))
    old_hash = hashlib.sha256(json.dumps(old_inputs, sort_keys=True).encode()).hexdigest()
    assert 'legacy-hash=' + old_hash in Path(env['GITHUB_OUTPUT']).read_text()
    assert fingerprint() == base
    env['TEST_PACKAGES'] = 'libc++-19-dev=2\nlibc6=1'
    assert fingerprint() == base, 'package order should not change the key'
    (temp/'source.c++').write_text('changed source')
    assert fingerprint() == base, 'source changes should retain toolchain cache'
    env['ImageVersion'] = '20261030'
    assert fingerprint() == base, 'runner image revision is not a native build input'
    env['TEST_PACKAGES'] += '\nfirefox=99\nazure-cli=1'
    assert fingerprint() == base, 'unrelated system packages must not invalidate outputs'
    env['ImageVersion'] = '20261029'
    env['TEST_PACKAGES'] = 'libc6=2\nlibc++-19-dev=2'
    assert fingerprint() != base
    env['TEST_PACKAGES'] = 'libc6=1\nlibc++-19-dev=2'
    (temp/'build/llvm/bin/clang').write_bytes(b'compiler-v2')
    assert fingerprint() != base
    (temp/'build/llvm/bin/clang').write_bytes(b'compiler-v1')
    pin=json.loads((temp/'upstream.json').read_text())
    pin['bazel']='9.2.0'
    (temp/'upstream.json').write_text(json.dumps(pin))
    assert fingerprint() != base
    for tag, expected in [('',0),('v'+pin['version'],0),('v999.0.0',1)]:
        result=subprocess.run(['bash','-e','-c',by_name['Verify release tag']['run']],cwd=temp,
                              env=dict(env, RELEASE_TAG=tag),capture_output=True)
        assert result.returncode == expected, (tag,result.stderr)
    dist=temp/'dist'; dist.mkdir()
    archive=dist/'isolate-host-0.1.0-linux-x86_64.tar.xz'; archive.write_bytes(b'test archive')
    (dist/'SHA256SUMS').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n')
    (dist/'manifest.json').write_text('{}')
    gh=temp/'fake-bin/gh'
    gh.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$TEST_GH_ARGS"\n')
    gh.chmod(0o755)
    env.update(TEST_GH_ARGS=str(temp/'gh-args'), RELEASE_TAG='v0.1.0')
    publish=workflow['jobs']['publish']['steps'][-1]['run']
    subprocess.run(['bash','-e','-o','pipefail','-c',publish],cwd=dist,env=env,check=True,capture_output=True)
    args=(temp/'gh-args').read_text().splitlines()
    assert args[:3] == ['release','create','v0.1.0']
    assert archive.name in args and '--verify-tag' in args
    (temp/'gh-args').unlink()
    archive.write_bytes(b'corrupt')
    result=subprocess.run(['bash','-e','-o','pipefail','-c',publish],cwd=dist,env=env,capture_output=True)
    assert result.returncode != 0 and not (temp/'gh-args').exists()
print('Passed workflow shell syntax, cache fingerprint/invalidation, tag validation, and publication checksum-gate checks.')
