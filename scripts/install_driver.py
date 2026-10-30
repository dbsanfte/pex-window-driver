#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Transactional DKMS/initramfs installation with injectable system operations.

The production CLI requires root. Tests inject a temporary filesystem root and
command runner, exercising identical compensation and state-publication logic.
"""
import argparse
import fcntl
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

CONFIG_FILES = (
    ('initramfs/pex-window.conf', '/etc/modprobe.d/pex-window.conf', 0o644),
    ('initramfs/hook', '/etc/initramfs-tools/hooks/pex-window', 0o755),
    ('initramfs/00-pex-window', '/etc/initramfs-tools/scripts/init-top/00-pex-window', 0o755),
)
SOURCE_FILES = ('pex_window.c', 'plan.h', 'transaction.h', 'Makefile', 'VERSION', 'dkms.conf')


def execute(arguments):
    """Run a system command, propagate failures, and return captured output."""
    result = subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'{arguments!r} failed ({result.returncode}):\n{result.stdout}')
    if arguments[:2] != ['dkms', 'status'] and arguments[0] != 'lsinitramfs':
        print(result.stdout, end='', flush=True)
    return result.stdout


class Installation:
    """Own one install/uninstall transaction without assuming a global filesystem."""

    def __init__(self, source, root=Path('/'), runner=execute, copier=shutil.copyfile):
        """Inject source tree, filesystem root, commands, and file-copy fault points."""
        self.source = Path(source)
        self.root = Path(root)
        self.run = runner
        self.copy = copier
        self.state = self.path('/var/lib/pex-window/install-state')

    def path(self, absolute):
        """Translate a system path into the selected filesystem root."""
        return self.root / str(absolute).lstrip('/')

    def registered(self, version):
        """Report DKMS registration, including a partially created version directory."""
        status = self.run(['dkms', 'status', '-m', 'pex-window', '-v', version]).strip()
        return bool(status) or self.path(f'/var/lib/dkms/pex-window/{version}').exists()

    @staticmethod
    def validate_version(version):
        """Reject version strings that could escape managed filesystem paths."""
        if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
            raise ValueError('Expected a numeric major.minor.patch version')

    @staticmethod
    def atomic_copy(source, destination):
        """Replace an initramfs only after its complete backup copy is available."""
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix='.pex-restore-', delete=False) as temporary:
            temp = Path(temporary.name)
        try:
            shutil.copy2(source, temp)
            os.replace(temp, destination)
        finally:
            temp.unlink(missing_ok=True)

    def install(self, kernel):
        """Build and install, publishing state only after all boot checks succeed.

        Any failed stage removes only this attempt's files and DKMS registration,
        then atomically restores the original initramfs. Cleanup errors are
        reported explicitly instead of claiming a clean rollback.
        """
        version = (self.source / 'VERSION').read_text().strip()
        self.validate_version(version)
        if not re.fullmatch(r'[A-Za-z0-9._+-]+', kernel):
            raise ValueError('Invalid kernel release')
        source_dir = self.path(f'/usr/src/pex-window-{version}')
        image = self.path(f'/boot/initrd.img-{kernel}')
        if not self.path(f'/lib/modules/{kernel}/build').is_dir() or not image.is_file():
            raise RuntimeError('Target kernel headers and initramfs must already exist')
        managed = [self.path(destination) for _, destination, _ in CONFIG_FILES]
        if self.state.exists() or source_dir.exists() or any(p.exists() for p in managed):
            raise RuntimeError('Existing installation/files found; uninstall before replacing')
        if self.registered(version) or self.run(['dkms', 'status', '-m', 'pex-window']).strip():
            raise RuntimeError('Existing DKMS registration found; uninstall before replacing')
        backups = self.path('/var/lib/pex-window/backups')
        backups.mkdir(parents=True, exist_ok=True)
        backup = Path(tempfile.mkdtemp(prefix=kernel + '.', dir=backups))
        shutil.copy2(image, backup / 'initrd.img')
        created = []
        source_created = False
        add_attempted = False
        try:
            source_dir.mkdir(parents=True)
            source_created = True
            for name in SOURCE_FILES:
                self.copy(self.source / name, source_dir / name)
                (source_dir / name).chmod(0o644)
            add_attempted = True
            self.run(['dkms', 'add', '-m', 'pex-window', '-v', version])
            self.run(['dkms', 'build', '-m', 'pex-window', '-v', version, '-k', kernel])
            self.run(['dkms', 'install', '-m', 'pex-window', '-v', version, '-k', kernel])
            for source, destination, mode in CONFIG_FILES:
                dest = self.path(destination)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.touch(exist_ok=False)
                created.append(dest)
                self.copy(self.source / source, dest)
                dest.chmod(mode)
            self.run(['depmod', '-a', kernel])
            self.run(['update-initramfs', '-u', '-k', kernel])
            contents = self.run(['lsinitramfs', str(image)])
            if 'scripts/init-top/00-pex-window' not in contents or 'pex_window.ko' not in contents:
                raise RuntimeError('Boot image is missing the helper or early-loading script')
            (backup / 'installed-contents.txt').write_text(contents)
            with tempfile.NamedTemporaryFile(mode='w', dir=self.state.parent, prefix='.install-', delete=False) as state_file:
                temporary_state = Path(state_file.name)
                state_file.write(f'{version}\n{kernel}\n{backup}\n')
            try:
                os.replace(temporary_state, self.state)
            finally:
                temporary_state.unlink(missing_ok=True)
        except BaseException as failure:
            errors = []
            for dest in reversed(created):
                try:
                    dest.unlink(missing_ok=True)
                except OSError as error:
                    errors.append(str(error))
            dkms_clean = True
            if add_attempted:
                try:
                    if self.registered(version):
                        self.run(['dkms', 'remove', '-m', 'pex-window', '-v', version, '--all'])
                except Exception as error:
                    dkms_clean = False
                    errors.append(str(error))
            if source_created and dkms_clean:
                try:
                    shutil.rmtree(source_dir)
                except OSError as error:
                    errors.append(str(error))
            try:
                self.atomic_copy(backup / 'initrd.img', image)
            except OSError as error:
                errors.append(str(error))
            if errors:
                raise RuntimeError(f'Installation failed: {failure}; cleanup incomplete: {errors}; backup: {backup}') from failure
            raise RuntimeError(f'Installation failed and additions were removed: {failure}; original initramfs restored from {backup}') from failure
        return backup

    def uninstall(self):
        """Remove integration idempotently, retaining state until image rebuild succeeds."""
        if not self.state.exists():
            return
        version, kernel, backup = self.state.read_text().splitlines()
        self.validate_version(version)
        for _, destination, _ in CONFIG_FILES:
            self.path(destination).unlink(missing_ok=True)
        if self.registered(version):
            self.run(['dkms', 'remove', '-m', 'pex-window', '-v', version, '--all'])
        self.run(['update-initramfs', '-u', '-k', 'all'])
        if self.path('/sys/module/pex_window').exists():
            self.run(['rmmod', 'pex_window'])
        source_dir = self.path(f'/usr/src/pex-window-{version}')
        if source_dir.exists():
            shutil.rmtree(source_dir)
        self.state.unlink()


def main():
    """Run the privileged installer under an exclusive host-local lock."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['install', 'uninstall'])
    parser.add_argument('kernel', nargs='?', default=os.uname().release)
    args = parser.parse_args()
    if os.geteuid():
        raise RuntimeError('Run as root')
    with open('/run/pex-window-install.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        installation = Installation(Path(__file__).resolve().parents[1])
        if args.mode == 'install':
            backup = installation.install(args.kernel)
            print(f'Installed for {args.kernel}. Original initramfs: {backup}/initrd.img')
            print('Live PCI state is unchanged by installation. Boot ordering needs the next boot.')
        else:
            installation.uninstall()
            print('Removed boot integration; live allocations persist until reboot. Backups retained.')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)
