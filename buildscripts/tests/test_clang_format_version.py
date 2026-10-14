"""Verify that lint detects and fixes drift in the editor formatter pin."""

import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock

from buildscripts.clang_format_version import check_version, main


class TestClangFormatVersion(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.pin = Path(self.tempdir.name) / "clang_format_version.txt"
        self.pin.write_text("MongoDB clang-format version 18.1.0\n")
        self.binary = Path("/toolchain/bin/clang-format")

    def check(self, version: str, *, fix: bool) -> bool:
        with (
            mock.patch(
                "buildscripts.clang_format_version.subprocess.run",
                return_value=subprocess.CompletedProcess([], 0, stdout=version),
            ) as run,
            redirect_stdout(StringIO()),
        ):
            result = check_version(self.binary, self.pin, fix=fix)
        run.assert_called_once_with(
            [str(self.binary), "--version"], capture_output=True, text=True, check=True
        )
        return result

    def test_check_detects_drift_without_modifying_pin(self) -> None:
        self.assertFalse(self.check("MongoDB clang-format version 19.1.7\n", fix=False))
        self.assertEqual(self.pin.read_text(), "MongoDB clang-format version 18.1.0\n")

    def test_fix_updates_pin_and_subsequent_check_passes(self) -> None:
        version = "MongoDB clang-format version 19.1.7\n"
        self.assertTrue(self.check(version, fix=True))
        self.assertEqual(self.pin.read_text(), version)
        self.assertTrue(self.check(version, fix=False))

    def test_missing_pin_can_be_created(self) -> None:
        self.pin.unlink()
        self.assertFalse(self.check("MongoDB clang-format version 19.1.7", fix=False))
        self.assertTrue(self.check("MongoDB clang-format version 19.1.7", fix=True))

    def test_invalid_version_output_does_not_replace_pin(self) -> None:
        for version in ("", "noop", "clang-format version 19.1.7\nextra output"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.check(version, fix=True)
        self.assertEqual(self.pin.read_text(), "MongoDB clang-format version 18.1.0\n")

    def test_failed_probe_does_not_replace_pin(self) -> None:
        with (
            mock.patch(
                "buildscripts.clang_format_version.subprocess.run",
                side_effect=subprocess.CalledProcessError(23, [str(self.binary), "--version"]),
            ),
            self.assertRaises(subprocess.CalledProcessError),
        ):
            check_version(self.binary, self.pin, fix=True)
        self.assertEqual(self.pin.read_text(), "MongoDB clang-format version 18.1.0\n")

    def test_lint_prepares_editor_cache_without_modifying_matching_pin(self) -> None:
        # Use a relative path so this test exercises resolution on every host,
        # including Windows adding the current drive to a drive-less path.
        self.binary = Path("toolchain/bin/clang-format")
        root = Path(self.tempdir.name)
        (root / "buildscripts").mkdir()
        pin = root / "buildscripts/clang_format_version.txt"
        version = "MongoDB clang-format version 19.1.7\n"
        pin.write_text(version)
        with (
            mock.patch.dict("os.environ", {"BUILD_WORKSPACE_DIRECTORY": str(root)}),
            mock.patch(
                "sys.argv", ["version-check", "--clang-format", str(self.binary), "--check"]
            ),
            mock.patch(
                "buildscripts.clang_format_version.subprocess.run",
                return_value=subprocess.CompletedProcess([], 0, stdout=version),
            ),
        ):
            self.assertEqual(main(), 0)
        self.assertEqual(pin.read_text(), version)
        self.assertEqual(
            (root / ".cache/clang-format-vscode").read_text(), f"{self.binary.resolve()}\n"
        )


if __name__ == "__main__":
    unittest.main()
