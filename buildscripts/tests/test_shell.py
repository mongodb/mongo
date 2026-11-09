"""Tests for selecting the shell used by Evergreen integration fixtures."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from buildscripts.tests.shell import bash_executable


class ShellTest(unittest.TestCase):
    def test_windows_uses_cygwin_even_when_bash_on_path_is_wsl(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cygpath = root / "cygpath.exe"
            cygpath.touch()
            bash = root / "bash.exe"
            bash.touch()
            with (
                patch("buildscripts.tests.shell.sys.platform", "win32"),
                patch("buildscripts.tests.shell.shutil.which", return_value=str(cygpath)) as which,
            ):
                self.assertEqual(str(bash), bash_executable())
                which.assert_called_once_with("cygpath")

    def test_windows_does_not_fall_back_to_wsl(self) -> None:
        with (
            patch("buildscripts.tests.shell.sys.platform", "win32"),
            patch("buildscripts.tests.shell.shutil.which", return_value=None),
            patch.object(type(Path()), "is_file", return_value=False),
        ):
            with self.assertRaisesRegex(FileNotFoundError, "Cygwin Bash"):
                bash_executable()

    def test_other_hosts_use_bash_from_path(self) -> None:
        with (
            patch("buildscripts.tests.shell.sys.platform", "linux"),
            patch("buildscripts.tests.shell.shutil.which", return_value="/bin/bash"),
        ):
            self.assertEqual("/bin/bash", bash_executable())


if __name__ == "__main__":
    unittest.main()
