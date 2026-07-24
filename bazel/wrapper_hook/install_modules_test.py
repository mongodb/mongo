"""Unit tests for bazel.wrapper_hook.install_modules."""

import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import install_modules


def _make_venv_skeleton(root: pathlib.Path) -> pathlib.Path:
    """Create just enough of a venv tree for install_modules' probes."""
    if os.name == "nt":
        (root / "Scripts").mkdir(parents=True)
        (root / "Scripts" / "python.exe").touch()
        sp = root / "Lib" / "site-packages"
    else:
        (root / "bin").mkdir(parents=True)
        (root / "bin" / "python3").touch()
        sp = root / "lib" / "python3.13" / "site-packages"
    sp.mkdir(parents=True)
    return sp


class BootstrapModulesTest(unittest.TestCase):
    def test_second_phase_only_reapplies_python_path(self):
        with (
            mock.patch.dict(
                os.environ,
                {install_modules.MODULES_READY_ENV: "1"},
                clear=True,
            ),
            mock.patch.object(install_modules, "setup_python_path") as mock_setup,
            mock.patch.object(install_modules, "install_modules") as mock_install,
            mock.patch.object(install_modules.os, "execve") as mock_execve,
        ):
            install_modules.bootstrap_modules("bazel", ["bazel", "build", "//:lint"])

        mock_setup.assert_called_once_with()
        mock_install.assert_not_called()
        mock_execve.assert_not_called()

    def test_first_phase_returns_without_reexec_when_no_install_needed(self):
        with (
            mock.patch.dict(os.environ, {}, clear=True),
            mock.patch.object(
                install_modules, "install_modules", return_value=False
            ) as mock_install,
            mock.patch.object(install_modules.os, "execve") as mock_execve,
        ):
            install_modules.bootstrap_modules("bazel", ["bazel", "build", "//:lint"])

        mock_install.assert_called_once_with("bazel", ["bazel", "build", "//:lint"])
        mock_execve.assert_not_called()

    def test_first_phase_reexecs_after_nested_install(self):
        argv = ["wrapper_hook.py", "bazel", "build", "//:lint"]
        with (
            mock.patch.dict(os.environ, {}, clear=True),
            mock.patch.object(
                install_modules, "install_modules", return_value=True
            ) as mock_install,
            mock.patch.object(install_modules.os, "execve") as mock_execve,
            mock.patch.object(install_modules.sys, "executable", "/tmp/repo-python"),
            mock.patch.object(install_modules.sys, "argv", argv),
        ):
            install_modules.bootstrap_modules("bazel", argv[1:])

        mock_install.assert_called_once_with("bazel", argv[1:])
        mock_execve.assert_called_once()
        exec_path, exec_argv, exec_env = mock_execve.call_args.args
        self.assertEqual(exec_path, "/tmp/repo-python")
        self.assertEqual(exec_argv, ["/tmp/repo-python", *argv])
        self.assertEqual(exec_env[install_modules.MODULES_READY_ENV], "1")


class InstallModulesTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.venv = pathlib.Path(self._tmp.name) / "venv"
        self.site_packages = _make_venv_skeleton(self.venv)
        self._env = mock.patch.dict(os.environ, {"VIRTUAL_ENV": str(self.venv)}, clear=True)
        self._env.start()
        self.addCleanup(self._env.stop)

    def _stamp(self) -> pathlib.Path:
        return self.venv / install_modules._STAMP_NAME

    def test_stamp_match_skips_sync(self):
        self._stamp().write_text(install_modules._uv_lock_hash() + "\n", encoding="utf-8")
        with mock.patch.object(install_modules.subprocess, "run") as mock_run:
            self.assertFalse(install_modules.install_modules("bazel", []))
        mock_run.assert_not_called()

    def test_stale_stamp_syncs_and_rewrites_stamp(self):
        self._stamp().write_text("stale-hash\n", encoding="utf-8")
        with mock.patch.object(
            install_modules.subprocess,
            "run",
            return_value=mock.Mock(returncode=0),
        ) as mock_run:
            self.assertTrue(install_modules.install_modules("bazel", []))

        mock_run.assert_called_once()
        cmd = mock_run.call_args.args[0]
        self.assertIn("sync", cmd)
        self.assertIn("--only-group", cmd)
        self.assertIn("wrapper-hook", cmd)
        self.assertIn("--locked", cmd)
        self.assertIn("--inexact", cmd)
        self.assertEqual(mock_run.call_args.kwargs["env"]["UV_PROJECT_ENVIRONMENT"], str(self.venv))
        self.assertEqual(
            self._stamp().read_text(encoding="utf-8").strip(),
            install_modules._uv_lock_hash(),
        )

    def test_failed_sync_returns_false_and_writes_no_stamp(self):
        with (
            mock.patch.object(
                install_modules.subprocess,
                "run",
                return_value=mock.Mock(returncode=1),
            ),
            # No uv on PATH: only the venv-python candidate is attempted.
            mock.patch.object(install_modules.shutil, "which", return_value=None),
        ):
            self.assertFalse(install_modules.install_modules("bazel", []))
        self.assertFalse(self._stamp().exists())

    def test_missing_venv_bootstraps_via_uv_sync_sh(self):
        os.environ.pop("VIRTUAL_ENV")
        with (
            mock.patch.object(
                install_modules, "_target_venv", return_value=pathlib.Path(self._tmp.name) / "nope"
            ),
            mock.patch.object(
                install_modules.subprocess,
                "run",
                return_value=mock.Mock(returncode=0),
            ) as mock_run,
        ):
            self.assertTrue(install_modules.install_modules("bazel", []))
        cmd = mock_run.call_args.args[0]
        self.assertEqual(cmd[0], "bash")
        self.assertTrue(str(cmd[1]).endswith("uv_sync.sh"))
        self.assertEqual(cmd[2], "-f")

    def test_setup_python_path_appends_target_venv_site_packages(self):
        original = list(sys.path)
        try:
            install_modules.setup_python_path()
            self.assertIn(str(self.site_packages), sys.path)
        finally:
            sys.path[:] = original


if __name__ == "__main__":
    unittest.main()
