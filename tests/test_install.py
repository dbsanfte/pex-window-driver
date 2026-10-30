#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""CTest regressions for installer compensation, ownership, and retry behavior."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "install_driver", SOURCE / "scripts/install_driver.py"
)
INSTALL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALL)


class InstallerRecovery(unittest.TestCase):
    """Use the production installer with a disposable filesystem and fake commands."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pex-install-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.kernel = "test-kernel"
        self.version = (SOURCE / "VERSION").read_text().strip()
        self.image = self.root / "boot/initrd.img-test-kernel"
        self.image.parent.mkdir(parents=True)
        self.image.write_bytes(b"original boot image")
        (self.root / "lib/modules/test-kernel/build").mkdir(parents=True)
        self.dkms = self.root / f"var/lib/dkms/pex-window/{self.version}"
        self.fail = None
        self.calls = []
        self.installer = INSTALL.Installation(SOURCE, self.root, self.run_command, self.copy)
        self.unrelated = self.root / "unrelated"
        self.unrelated.write_text("keep")

    def run_command(self, arguments):
        self.calls.append(tuple(arguments))
        command = arguments[0]
        stage = arguments[1] if command == "dkms" else command
        failing = self.fail == stage or (isinstance(self.fail, set) and stage in self.fail)
        if command == "dkms":
            if stage == "status":
                return "registered" if self.dkms.exists() else ""
            if stage == "add":
                self.dkms.mkdir(parents=True)
            if stage == "remove":
                if failing:
                    raise RuntimeError("injected remove")
                shutil.rmtree(self.dkms)
        if stage in ("install", "update-initramfs"):
            self.image.write_bytes(b"new boot image")
        if failing:
            raise RuntimeError("injected " + stage)
        if command == "lsinitramfs":
            return "scripts/init-top/00-pex-window\nlib/modules/test/pex_window.ko.zst\n"
        return ""

    def copy(self, source, destination):
        shutil.copyfile(source, destination)
        if self.fail == "source-copy" and Path(destination).name == "pex_window.c":
            raise OSError("injected partial source copy")
        if self.fail == "config-copy" and "/etc/modprobe.d/" in str(destination):
            raise OSError("injected partial configuration copy")

    def assert_compensated(self):
        self.assertFalse(self.installer.state.exists())
        self.assertFalse(self.dkms.exists())
        self.assertFalse((self.root / f"usr/src/pex-window-{self.version}").exists())
        for _, destination, _ in INSTALL.CONFIG_FILES:
            self.assertFalse(self.installer.path(destination).exists())
        self.assertEqual(self.image.read_bytes(), b"original boot image")
        self.assertEqual(self.unrelated.read_text(), "keep")

    def test_failure_at_each_stage_removes_only_additions_and_allows_retry(self):
        for stage in (
            "source-copy",
            "add",
            "build",
            "install",
            "config-copy",
            "depmod",
            "update-initramfs",
            "lsinitramfs",
        ):
            with self.subTest(stage=stage):
                self.fail = stage
                with self.assertRaisesRegex(RuntimeError, "additions were removed"):
                    self.installer.install(self.kernel)
                self.assert_compensated()
        self.fail = None
        self.installer.install(self.kernel)
        self.assertTrue(self.installer.state.exists())
        self.assertTrue(self.dkms.exists())
        self.assertTrue((self.root / f"usr/src/pex-window-{self.version}/transaction.h").exists())

    def test_existing_state_is_preserved(self):
        self.installer.state.parent.mkdir(parents=True)
        self.installer.state.write_text("previous installation state")
        with self.assertRaisesRegex(RuntimeError, "Existing installation"):
            self.installer.install(self.kernel)
        self.assertEqual(self.installer.state.read_text(), "previous installation state")
        self.assertEqual(self.calls, [])
        self.assertEqual(self.image.read_bytes(), b"original boot image")

    def test_existing_source_is_not_removed(self):
        source = self.root / f"usr/src/pex-window-{self.version}"
        source.mkdir(parents=True)
        (source / "owner-data").write_text("preserve")
        with self.assertRaisesRegex(RuntimeError, "Existing installation"):
            self.installer.install(self.kernel)
        self.assertEqual((source / "owner-data").read_text(), "preserve")

    def test_state_is_absent_until_boot_validation_completes(self):
        runner = self.installer.run

        def check(arguments):
            self.assertFalse(self.installer.state.exists())
            return runner(arguments)

        self.installer.run = check
        self.installer.install(self.kernel)
        self.assertTrue(self.installer.state.exists())

    def test_uninstall_retries_after_image_update_failure(self):
        self.installer.install(self.kernel)
        self.fail = "update-initramfs"
        with self.assertRaisesRegex(RuntimeError, "injected"):
            self.installer.uninstall()
        self.assertTrue(self.installer.state.exists())
        self.assertFalse(self.dkms.exists())
        self.fail = None
        self.installer.uninstall()
        self.assertFalse(self.installer.state.exists())
        removes = [call for call in self.calls if call[:2] == ("dkms", "remove")]
        self.assertEqual(len(removes), 1)
        self.installer.uninstall()  # Already removed is a successful no-op.

    def test_cleanup_failure_is_reported_and_preserves_recovery_material(self):
        self.fail = {"build", "remove"}
        with self.assertRaisesRegex(RuntimeError, "cleanup incomplete"):
            self.installer.install(self.kernel)
        self.assertTrue(self.dkms.exists())
        self.assertTrue((self.root / f"usr/src/pex-window-{self.version}").exists())
        self.assertFalse(self.installer.state.exists())
        self.assertEqual(self.image.read_bytes(), b"original boot image")

    def test_invalid_version_is_rejected_before_filesystem_changes(self):
        with self.assertRaises(ValueError):
            INSTALL.Installation.validate_version("../../another-package")
        self.assert_compensated()


if __name__ == "__main__":
    unittest.main(verbosity=2)
