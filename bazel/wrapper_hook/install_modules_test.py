"""Tests for installing wrapper dependencies during cross compilation."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from bazel.wrapper_hook import install_modules


class InstallModulesTest(unittest.TestCase):
    def test_wrapper_dependencies_use_the_host_platform(self) -> None:
        for first_returncode in (0, 1):
            with (
                self.subTest(first_returncode=first_returncode),
                tempfile.TemporaryDirectory() as tmp,
            ):
                repo_root = Path(tmp)
                (repo_root / "poetry.lock").write_text("test lockfile", encoding="utf-8")
                with (
                    patch.object(install_modules, "REPO_ROOT", repo_root),
                    patch.object(install_modules.tempfile, "gettempdir", return_value=tmp),
                    patch.object(
                        install_modules, "search_for_modules", side_effect=[["pyyaml"], []]
                    ),
                    patch.object(install_modules, "setup_python_path"),
                    patch.object(install_modules.subprocess, "run") as run,
                ):
                    run.return_value.returncode = first_returncode
                    install_modules.install_modules(
                        "bazel", ["build", "--config=linux-s390x-rhel9-cross-gcc-arm64"]
                    )
                    self.assertEqual(run.call_count, 1 if first_returncode == 0 else 2)
                    for call in run.call_args_list:
                        self.assertIn(
                            "--platforms=@internal_platforms_do_not_use//host", call.args[0]
                        )


if __name__ == "__main__":
    unittest.main()
