"""Regression tests for container_engine."""

from __future__ import annotations

import pathlib
import subprocess
import unittest
from unittest import mock

from bazel.toolchains.cc import container_action as shared
from bazel.toolchains.cc import container_engine as helpers
from bazel.toolchains.cc.container_engine import DOCKER_HOST_MODE_ENV, WSL_LOCAL_DOCKER_HOST_MODE


class HelpersTest(unittest.TestCase):
    def test_selects_docker_desktop_linux_context_from_windows_engine(self):
        def docker_run(args, **kwargs):
            if "--context" in args:
                return subprocess.CompletedProcess(args, 0, stdout="linux\n", stderr="")
            if "context" in args:
                return subprocess.CompletedProcess(args, 0, stdout="desktop-linux\n", stderr="")
            return subprocess.CompletedProcess(args, 0, stdout="windows\n", stderr="")

        with (
            mock.patch.dict(helpers.os.environ, {}, clear=True),
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.object(helpers, "_wsl_docker_command", return_value=None),
            mock.patch.object(helpers, "_tcp_wsl_docker_command", return_value=None),
            mock.patch.object(shared, "run", side_effect=docker_run),
        ):
            self.assertEqual(
                helpers.select_linux_docker_command(["docker"]),
                ["docker", "--context", "desktop-linux"],
            )
            self.assertEqual(
                helpers.os.environ[DOCKER_HOST_MODE_ENV],
                helpers.DOCKER_DESKTOP_HOST_MODE,
            )

    def test_uses_wsl_linux_docker_engine_when_available(self):
        with (
            mock.patch.object(
                helpers,
                "_wsl_docker_command",
                return_value=["wsl.exe", "--exec", "docker"],
            ),
            mock.patch.object(
                shared,
                "docker_info_os_type",
                return_value="linux",
            ),
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(
                helpers.os.environ,
                {DOCKER_HOST_MODE_ENV: "wsl"},
                clear=True,
            ),
        ):
            self.assertEqual(
                helpers.select_linux_docker_command(["docker"]),
                ["wsl.exe", "--exec", "docker"],
            )
            self.assertEqual(
                helpers.os.environ[DOCKER_HOST_MODE_ENV],
                helpers.WSL_DOCKER_HOST_MODE,
            )

    def test_default_windows_mode_uses_same_distro_wsl_engine(self):
        local_docker = [
            "wsl.exe",
            "--exec",
            "env",
            "DOCKER_HOST=unix:///var/run/docker.sock",
            "DOCKER_CONTEXT=",
            "docker",
        ]

        with (
            mock.patch.object(
                helpers,
                "_wsl_docker_command",
                return_value=local_docker,
            ) as wsl_docker,
            mock.patch.object(
                helpers,
                "_ensure_wsl_local_docker_engine",
                return_value=(True, ""),
            ),
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(helpers.os.environ, {}, clear=True),
        ):
            self.assertEqual(
                helpers.select_linux_docker_command(["docker"]),
                local_docker,
            )
            self.assertEqual(
                helpers.os.environ.get(DOCKER_HOST_MODE_ENV),
                WSL_LOCAL_DOCKER_HOST_MODE,
            )

        wsl_docker.assert_called_once_with(local_engine=True)

    def test_default_windows_mode_falls_back_to_tcp_wsl_docker_engine(self):
        with (
            mock.patch.object(
                helpers,
                "_wsl_docker_command",
                return_value=None,
            ) as wsl_docker,
            mock.patch.object(
                shared,
                "docker_info_os_type",
                return_value="linux",
            ),
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(
                helpers.os.environ,
                {"DOCKER_CONTEXT": "desktop-linux"},
                clear=True,
            ),
        ):
            self.assertEqual(
                helpers.select_linux_docker_command(["docker"]),
                ["docker"],
            )
            self.assertEqual(
                helpers.os.environ.get(DOCKER_HOST_MODE_ENV),
                helpers.WSL_DOCKER_HOST_MODE,
            )
            self.assertEqual(
                helpers.os.environ.get("DOCKER_HOST"),
                helpers.WSL_TCP_DOCKER_HOST_DEFAULT,
            )
            self.assertNotIn("DOCKER_CONTEXT", helpers.os.environ)

        wsl_docker.assert_called_once_with(local_engine=True)

    def test_default_windows_mode_skips_tcp_wsl_docker_when_unreachable(self):
        with (
            mock.patch.object(
                helpers,
                "_wsl_docker_command",
                return_value=None,
            ),
            mock.patch.object(
                shared,
                "docker_info_os_type",
                return_value="",
            ),
            mock.patch.object(
                shared,
                "run",
                return_value=subprocess.CompletedProcess([], 1, stdout="", stderr=""),
            ),
            mock.patch.object(
                helpers,
                "_switch_docker_desktop_to_linux",
                return_value=None,
            ) as switch_desktop,
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(
                helpers.os.environ,
                {"DOCKER_HOST": "tcp://127.0.0.1:1", "DOCKER_CONTEXT": "bogus"},
                clear=True,
            ),
        ):
            self.assertIsNone(helpers.select_linux_docker_command(["docker"]))
            # The caller's Docker routing is restored after the failed probe.
            self.assertEqual(
                helpers.os.environ.get("DOCKER_HOST"),
                "tcp://127.0.0.1:1",
            )
            self.assertEqual(helpers.os.environ.get("DOCKER_CONTEXT"), "bogus")
            self.assertNotIn(
                DOCKER_HOST_MODE_ENV,
                helpers.os.environ,
            )

        switch_desktop.assert_called_once()

    def test_tcp_probe_preserves_native_routing(self):
        for command, host in (
            (["docker", "--context", "desktop-linux"], None),
            (["docker", "--context=desktop-linux"], "tcp://127.0.0.1:2375"),
            (["docker", "-H", "npipe:////./pipe/docker_engine"], None),
            (["docker", "--host=unix:///var/run/docker.sock"], None),
            (["docker"], "npipe:////./pipe/docker_engine"),
            (["docker"], "unix:///var/run/docker.sock"),
        ):
            environment = {"DOCKER_CONTEXT": "desktop-linux"}
            if host is not None:
                environment["DOCKER_HOST"] = host
            with (
                self.subTest(command=command, host=host),
                mock.patch.dict(helpers.os.environ, environment, clear=True),
                mock.patch.object(shared, "docker_info_os_type", return_value="linux") as info,
            ):
                self.assertIsNone(helpers._tcp_wsl_docker_command(command))
                self.assertEqual(dict(helpers.os.environ), environment)
                info.assert_not_called()

    def test_native_windows_fallback_keeps_mount_routing(self):
        for command, environment in (
            (["docker", "--context=desktop-linux"], {"DOCKER_CONTEXT": "desktop-linux"}),
            (["docker"], {"DOCKER_HOST": "npipe:////./pipe/docker_engine"}),
        ):
            with (
                self.subTest(command=command),
                mock.patch.dict(helpers.os.environ, environment, clear=True),
                mock.patch.object(helpers.platform, "system", return_value="Windows"),
                mock.patch.object(helpers, "_wsl_docker_command", return_value=None),
                mock.patch.object(shared, "docker_info_os_type", return_value="linux"),
            ):
                self.assertEqual(helpers.select_linux_docker_command(command), command)
                self.assertEqual(dict(helpers.os.environ), environment)

    def test_same_distro_wsl_command_selects_distribution_and_socket(self):
        with (
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.object(pathlib.Path, "is_file", return_value=True),
            mock.patch.dict(
                helpers.os.environ,
                {
                    helpers.WSL_DISTRIBUTION_ENV: "Ubuntu-24.04",
                    helpers.WSL_DOCKER_SOCKET_ENV: "unix:///run/docker.sock",
                },
                clear=True,
            ),
        ):
            self.assertEqual(
                helpers._wsl_docker_command(local_engine=True),
                [
                    str(pathlib.Path(r"C:\Windows") / "System32" / "wsl.exe"),
                    "--distribution",
                    "Ubuntu-24.04",
                    "--exec",
                    "env",
                    "DOCKER_HOST=unix:///run/docker.sock",
                    "DOCKER_CONTEXT=",
                    "docker",
                ],
            )

    def test_wsl_local_engine_starts_system_service(self):
        docker = [
            "wsl.exe",
            "--exec",
            "env",
            "DOCKER_HOST=unix:///var/run/docker.sock",
            "DOCKER_CONTEXT=",
            "docker",
        ]
        run_results = [
            subprocess.CompletedProcess([], 0, stdout="/usr/bin/dockerd\n", stderr=""),
            subprocess.CompletedProcess([], 0, stdout="", stderr=""),
        ]

        with (
            mock.patch.object(
                shared,
                "docker_info_os_type",
                side_effect=["", "linux"],
            ),
            mock.patch.object(
                helpers,
                "_wsl_local_docker_ready",
                side_effect=[False, True],
            ),
            mock.patch.object(
                helpers,
                "_wsl_command_prefix",
                return_value=["wsl.exe"],
            ),
            mock.patch.object(shared, "run", side_effect=run_results) as run,
            mock.patch.dict(helpers.os.environ, {}, clear=True),
        ):
            self.assertEqual(
                helpers._ensure_wsl_local_docker_engine(docker),
                (True, ""),
            )

        self.assertEqual(
            run.call_args_list[1].args[0],
            ["wsl.exe", "--exec", "sudo", "-n", "systemctl", "start", "docker"],
        )

    def test_wsl_local_engine_restarts_system_service(self):
        docker = ["wsl.exe", "--exec", "env", "DOCKER_HOST=unix:///var/run/docker.sock", "docker"]
        with (
            mock.patch.object(
                helpers,
                "_wsl_local_docker_ready",
                side_effect=[False, True],
            ),
            mock.patch.object(
                helpers,
                "_wsl_command_prefix",
                return_value=["wsl.exe"],
            ),
            mock.patch.object(
                shared,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout="", stderr=""),
            ) as run,
            mock.patch.dict(helpers.os.environ, {}, clear=True),
        ):
            self.assertEqual(
                helpers.restart_wsl_local_docker_engine(docker),
                (True, ""),
            )

        self.assertEqual(
            run.call_args.args[0],
            ["wsl.exe", "--exec", "sudo", "-n", "systemctl", "restart", "docker"],
        )

    def test_wsl_local_engine_rejects_docker_desktop_socket(self):
        docker = [
            "wsl.exe",
            "--exec",
            "env",
            "DOCKER_HOST=unix:///var/run/docker.sock",
            "docker",
        ]
        with (
            mock.patch.object(
                shared,
                "docker_info_os_type",
                return_value="linux",
            ),
            mock.patch.object(
                helpers,
                "_docker_daemon_is_docker_desktop",
                return_value=True,
            ),
            mock.patch.object(
                helpers,
                "_wsl_command_prefix",
                return_value=["wsl.exe"],
            ),
            mock.patch.object(
                shared,
                "run",
                return_value=subprocess.CompletedProcess(
                    [], 1, stdout="", stderr="dockerd unavailable"
                ),
            ),
            mock.patch.object(helpers.time, "monotonic", side_effect=[0, 31]),
            mock.patch.dict(helpers.os.environ, {}, clear=True),
        ):
            ready, detail = helpers._ensure_wsl_local_docker_engine(docker)

        self.assertFalse(ready)
        self.assertEqual(detail, "dockerd unavailable")

    def test_switches_docker_desktop_engine_when_linux_context_is_unavailable(self):
        def docker_run(args, **kwargs):
            if args[1:4] == ["context", "ls", "--format"]:
                return subprocess.CompletedProcess(args, 0, stdout="desktop-windows\n", stderr="")
            if args[1:5] == ["desktop", "engine", "use", "linux"]:
                return subprocess.CompletedProcess(args, 0, stdout="", stderr="")
            if args[0] == "wsl.exe":
                return subprocess.CompletedProcess(args, 0, stdout="linux\n", stderr="")
            if "--context" in args and "default" in args:
                return subprocess.CompletedProcess(args, 0, stdout="linux\n", stderr="")
            return subprocess.CompletedProcess(args, 0, stdout="windows\n", stderr="")

        with (
            mock.patch.dict(helpers.os.environ, {}, clear=True),
            mock.patch.object(
                helpers,
                "_wsl_docker_command",
                return_value=["wsl.exe", "--exec", "docker"],
            ),
            mock.patch.object(
                helpers,
                "_ensure_wsl_local_docker_engine",
                return_value=(False, "test daemon unavailable"),
            ),
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.object(shared, "run", side_effect=docker_run),
        ):
            self.assertEqual(
                helpers.select_linux_docker_command(["docker"]),
                ["docker", "--context", "default"],
            )
            self.assertEqual(
                helpers.os.environ[DOCKER_HOST_MODE_ENV],
                helpers.DOCKER_DESKTOP_HOST_MODE,
            )

    def test_startup_recovers_local_wsl_once(self):
        start = mock.Mock(side_effect=[SystemExit(1), None])
        with (
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(helpers.os.environ, {DOCKER_HOST_MODE_ENV: "wsl-local"}, clear=True),
            mock.patch.object(
                helpers, "restart_wsl_local_docker_engine", return_value=(True, "")
            ) as restart,
        ):
            self.assertTrue(helpers.start_container_with_recovery(["docker"], start))
        self.assertEqual(start.call_count, 2)
        restart.assert_called_once_with(["docker"])

    def test_tcp_startup_retry_does_not_restart_other_users_service(self):
        start = mock.Mock(side_effect=[SystemExit(1), None])
        with (
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(
                helpers.os.environ,
                {DOCKER_HOST_MODE_ENV: "wsl", "DOCKER_HOST": "tcp://127.0.0.1:2375"},
                clear=True,
            ),
            mock.patch.object(shared, "docker_info_os_type", return_value="linux"),
            mock.patch.object(helpers, "restart_wsl_local_docker_engine") as restart,
        ):
            self.assertTrue(helpers.start_container_with_recovery(["docker"], start))
        restart.assert_not_called()
        self.assertEqual(start.call_count, 2)

    def test_non_wsl_startup_failure_is_preserved(self):
        with mock.patch.object(helpers.platform, "system", return_value="Darwin"):
            with self.assertRaises(SystemExit) as raised:
                helpers.start_container_with_recovery(
                    ["docker"], mock.Mock(side_effect=SystemExit(7))
                )
        self.assertEqual(raised.exception.code, 7)

    def test_wsl_probe_timeout_is_reported(self):
        with (
            mock.patch.object(helpers, "_wsl_command_prefix", return_value=["wsl.exe"]),
            mock.patch.object(shared, "run", side_effect=subprocess.TimeoutExpired("wsl", 30)),
        ):
            ready, detail = helpers._ensure_wsl_local_docker_engine(["docker"])
        self.assertFalse(ready)
        self.assertIn("timed out", detail)


if __name__ == "__main__":
    unittest.main()
