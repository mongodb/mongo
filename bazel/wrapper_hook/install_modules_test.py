"""Unit tests for bazel.wrapper_hook.install_modules."""

import os
import unittest
from unittest import mock

from bazel.wrapper_hook import install_modules


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


if __name__ == "__main__":
    unittest.main()
