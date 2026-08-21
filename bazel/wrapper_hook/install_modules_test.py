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

    def test_first_phase_exits_for_mixed_platform_bootstrap(self):
        with (
            mock.patch.dict(os.environ, {}, clear=True),
            mock.patch.object(
                install_modules,
                "install_modules",
                side_effect=install_modules.MixedPlatformError("mixed platform"),
            ),
        ):
            with self.assertRaises(SystemExit) as raised:
                install_modules.bootstrap_modules("bazel", ["bazel", "build", "//:lint"])

        self.assertEqual(raised.exception.code, 1)


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

    def test_windows_bash_rejects_wsl(self):
        with (
            mock.patch.object(install_modules.os, "name", "nt"),
            mock.patch.object(
                install_modules,
                "_windows_bash_candidates",
                return_value=[r"C:\Windows\System32\bash.exe"],
            ),
            mock.patch.object(
                install_modules.subprocess,
                "run",
                return_value=mock.Mock(returncode=0, stdout="Linux\n"),
            ) as mock_run,
        ):
            with self.assertRaisesRegex(install_modules.MixedPlatformError, "WSL"):
                install_modules._check_windows_bash(self.venv)

        mock_run.assert_called_once_with(
            [r"C:\Windows\System32\bash.exe", "-c", "uname -s"],
            cwd=str(self.venv),
            capture_output=True,
            text=True,
        )

    def test_windows_bash_accepts_git_bash(self):
        with (
            mock.patch.object(install_modules.os, "name", "nt"),
            mock.patch.object(
                install_modules,
                "_windows_bash_candidates",
                return_value=[r"C:\Program Files\Git\bin\bash.exe"],
            ),
            mock.patch.object(
                install_modules.subprocess,
                "run",
                return_value=mock.Mock(returncode=0, stdout="MINGW64_NT-10.0\n"),
            ),
        ):
            self.assertEqual(
                install_modules._check_windows_bash(self.venv),
                r"C:\Program Files\Git\bin\bash.exe",
            )

    def test_windows_bash_skips_wsl_and_finds_git_bash(self):
        candidates = [
            r"C:\Windows\System32\bash.exe",
            r"C:\Program Files\Git\bin\bash.exe",
        ]
        with (
            mock.patch.object(install_modules.os, "name", "nt"),
            mock.patch.object(install_modules, "_windows_bash_candidates", return_value=candidates),
            mock.patch.object(
                install_modules.subprocess,
                "run",
                side_effect=[
                    mock.Mock(returncode=0, stdout="Linux\n"),
                    mock.Mock(returncode=0, stdout="MINGW64_NT-10.0\n"),
                ],
            ),
        ):
            self.assertEqual(install_modules._check_windows_bash(self.venv), candidates[1])

    def test_windows_venv_rejects_posix_layout(self):
        posix_venv = pathlib.Path(self._tmp.name) / "wsl-venv"
        (posix_venv / "bin").mkdir(parents=True)
        (posix_venv / "bin" / "python3").touch()
        with mock.patch.object(install_modules.os, "name", "nt"):
            with self.assertRaisesRegex(install_modules.MixedPlatformError, "POSIX/WSL"):
                install_modules._check_venv_layout(posix_venv)

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
        missing_venv = pathlib.Path(self._tmp.name) / "nope"

        def fake_bootstrap(*_args, **_kwargs):
            (missing_venv / "bin").mkdir(parents=True, exist_ok=True)
            (missing_venv / "bin" / "python3").touch()
            return mock.Mock(returncode=0)

        with (
            mock.patch.object(install_modules, "_target_venv", return_value=missing_venv),
            mock.patch.object(
                install_modules.subprocess,
                "run",
                side_effect=fake_bootstrap,
            ) as mock_run,
        ):
            self.assertTrue(install_modules.install_modules("bazel", []))
        cmd = mock_run.call_args.args[0]
        self.assertEqual(cmd[0], "bash")
        expected_uv_sync_sh = install_modules.REPO_ROOT.resolve() / "buildscripts" / "uv_sync.sh"
        self.assertEqual(
            cmd[1], expected_uv_sync_sh.relative_to(install_modules.REPO_ROOT.resolve()).as_posix()
        )
        self.assertEqual(cmd[2], "-f")
        self.assertEqual(mock_run.call_args.kwargs["cwd"], str(install_modules.REPO_ROOT.resolve()))

    def test_windows_bootstrap_uses_native_python_and_ignores_stale_venv(self):
        missing_venv = pathlib.Path(self._tmp.name) / "nope"

        def fake_bootstrap(*_args, **_kwargs):
            (missing_venv / "Scripts").mkdir(parents=True, exist_ok=True)
            (missing_venv / "Scripts" / "python.exe").touch()
            return mock.Mock(returncode=0)

        with (
            mock.patch.object(install_modules, "_target_venv", return_value=missing_venv),
            mock.patch.object(install_modules.os, "name", "nt"),
            mock.patch.object(
                install_modules,
                "_check_windows_bash",
                return_value=r"C:\Program Files\Git\bin\bash.exe",
            ),
            mock.patch.object(install_modules.sys, "executable", r"C:\pyhost\python.exe"),
            mock.patch.object(
                install_modules.subprocess,
                "run",
                side_effect=fake_bootstrap,
            ) as mock_run,
        ):
            self.assertTrue(install_modules.install_modules("bazel", []))

        self.assertEqual(
            mock_run.call_args.args[0],
            [r"C:\Program Files\Git\bin\bash.exe", "buildscripts/uv_sync.sh", "-f"],
        )
        bootstrap_env = mock_run.call_args.kwargs["env"]
        self.assertNotIn("VIRTUAL_ENV", bootstrap_env)
        self.assertEqual(bootstrap_env["PYTHON3"], "C:/pyhost/python.exe")

    def test_setup_python_path_appends_target_venv_site_packages(self):
        original = list(sys.path)
        try:
            install_modules.setup_python_path()
            self.assertIn(str(self.site_packages), sys.path)
        finally:
            sys.path[:] = original


if __name__ == "__main__":
    unittest.main()
