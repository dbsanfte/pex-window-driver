#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare hash-pinned kernel headers inside an isolated CI container.

Packages are extracted, never installed: no kernel, bootloader, or DKMS hooks run.
The resulting Module.symvers supplies real kernel ABI symbol CRCs for modpost.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import urllib.request


def fetch(url, expected, directory):
    """Download a header archive and require its checked-in SHA-256 digest."""
    destination = directory / url.rsplit('/', 1)[1]
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=90) as response:
                contents = response.read()
            break
        except OSError:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)
    if hashlib.sha256(contents).hexdigest() != expected:
        raise RuntimeError(f'Header checksum mismatch: {url}')
    destination.write_bytes(contents)
    return destination


def main():
    """Extract one configured ABI into a requested filesystem root."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['pinned', 'mainline6', 'mainline7'])
    parser.add_argument('--root', type=Path, required=True,
                        help='Extraction root; use / only inside a disposable CI container')
    parser.add_argument('--cache', type=Path, default=Path('build/header-debs'))
    args = parser.parse_args()
    manifest = Path(__file__).resolve().parents[1] / 'ci/kernel-targets.json'
    target = json.loads(manifest.read_text())[args.target]
    args.cache.mkdir(parents=True, exist_ok=True)
    args.root.mkdir(parents=True, exist_ok=True)
    for package in target['headers']:
        archive = fetch(package['url'], package['sha256'], args.cache)
        subprocess.run(['dpkg-deb', '-x', str(archive), str(args.root)], check=True)
    headers = args.root / 'usr/src' / f'linux-headers-{target["release"]}'
    if not (headers / 'Module.symvers').stat().st_size:
        raise RuntimeError('Missing symbol-version table')
    release = (headers / 'include/config/kernel.release').read_text().strip()
    if release != target['release']:
        raise RuntimeError(f'Unexpected kernel release: {release}')
    print(headers)


if __name__ == '__main__':
    main()
