#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package a module with its exact ABI identity, configuration, and build inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


def output(*command):
    """Run a metadata query and return its stripped standard output."""
    return subprocess.check_output(command, text=True).strip()


def main():
    """Validate vermagic and produce one ABI-specific archive and checksum."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['pinned', 'mainline6', 'mainline7'])
    parser.add_argument('--headers', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    target = json.loads((source / 'ci/kernel-targets.json').read_text())[args.target]
    module = source / 'pex_window.ko'
    version = (source / 'VERSION').read_text().strip()
    vermagic = output('modinfo', '-F', 'vermagic', str(module))
    if vermagic.split()[0] != target['release']:
        raise RuntimeError(f'Wrong ABI: {vermagic}')
    if output('modinfo', '-F', 'version', str(module)) != version:
        raise RuntimeError('Module version does not match source version')
    metadata = dict(target, target=args.target, module_version=version, vermagic=vermagic,
                    commit=output('git', '-C', str(source), 'rev-parse', 'HEAD'),
                    compiler_version=output(target['compiler'], '--version').splitlines()[0],
                    configuration_sha256=hashlib.sha256((args.headers / '.config').read_bytes()).hexdigest())
    prefix = f'pex-window-{version}-{target["release"]}-x86_64'
    dist = source / 'dist'
    dist.mkdir(exist_ok=True)
    archive = dist / f'{prefix}.tar.xz'
    with tempfile.TemporaryDirectory(prefix='pex-binary-') as temporary:
        package = Path(temporary) / prefix
        package.mkdir()
        shutil.copy2(module, package / 'pex_window.ko')
        shutil.copy2(args.headers / '.config', package / 'kernel.config')
        shutil.copy2(args.headers / 'Module.symvers', package / 'kernel.Module.symvers')
        shutil.copy2(source / 'LICENSE', package / 'LICENSE')
        (package / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
        (package / 'README.txt').write_text(
            f'pex-window {version} for {target["release"]} x86_64\n'
            'This unsigned module is specific to the recorded kernel ABI/configuration.\n'
            'It is not a generic Linux 6.x/7.x binary. Prefer the DKMS source archive\n'
            'for other kernels, local signing, and automatic rebuilds. See the source\n'
            'README for pre-driver loading, validation, and rollback instructions.\n')
        with tarfile.open(archive, 'w:xz') as tar:
            tar.add(package, arcname=prefix)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(archive.suffix + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(archive)


if __name__ == '__main__':
    main()
