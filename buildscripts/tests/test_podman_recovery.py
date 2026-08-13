from __future__ import annotations

import contextlib
import io
import subprocess
import unittest
from unittest import mock

from buildscripts import podman_recovery


def _result(
    argv: list[str],
    returncode: int,
    *,
    stdout: str = "",
    stderr: str = "",
) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(argv, returncode, stdout=stdout, stderr=stderr)


class PodmanRecoveryTest(unittest.TestCase):
    def test_non_podman_failure_is_not_recovered(self) -> None:
        failure = _result(["docker", "info"], 1, stderr="daemon unavailable")
        with mock.patch.object(
            podman_recovery.subprocess,
            "run",
            return_value=failure,
        ) as run:
            self.assertEqual(podman_recovery.run_with_recovery(["docker", "info"]), 1)

        run.assert_called_once()

    def test_non_stale_podman_failure_is_not_recovered(self) -> None:
        failure = _result(["podman", "run"], 125, stderr="newuidmap not found")
        with mock.patch.object(
            podman_recovery.subprocess,
            "run",
            return_value=failure,
        ) as run:
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 125)

        run.assert_called_once()

    def test_stale_podman_runtime_is_not_migrated(self) -> None:
        stale = (
            'invalid internal status, try resetting the pause process with "podman system migrate"'
        )
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
        ]
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(
                podman_recovery.subprocess,
                "run",
                side_effect=results,
            ) as run,
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 125)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                ["podman", "run"],
                ["podman", "info"],
            ],
        )
        self.assertIn("invalid internal status", stdout.getvalue() + stderr.getvalue())
        self.assertIn("stops all running containers", stderr.getvalue())
        self.assertIn("Checking whether another process has recovered", stderr.getvalue())

    def test_healthy_runtime_after_lock_skips_recovery(self) -> None:
        stale = _result(
            ["podman", "run"],
            125,
            stderr="invalid internal status",
        )
        results = [
            stale,
            _result(["podman", "info"], 0),
            _result(["podman", "run"], 0),
        ]
        with (
            mock.patch.object(
                podman_recovery.subprocess,
                "run",
                side_effect=results,
            ) as run,
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 0)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [["podman", "run"], ["podman", "info"], ["podman", "run"]],
        )

    def test_quiet_recovery_keeps_successful_command_output_clean(self) -> None:
        results = [
            _result(["podman", "run"], 125, stderr="invalid internal status"),
            _result(["podman", "info"], 0),
            _result(["podman", "run"], 0, stdout='{"success": true}\n'),
        ]
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results),
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(
                podman_recovery.run_with_recovery(
                    ["podman", "run"],
                    quiet_recovery=True,
                ),
                0,
            )

        self.assertEqual(stdout.getvalue(), '{"success": true}\n')
        self.assertEqual(stderr.getvalue(), "")

    def test_quiet_recovery_reports_stale_runtime_without_migrating(self) -> None:
        stale = _result(["podman", "run"], 125, stderr="invalid internal status")
        results = [stale, _result(["podman", "info"], 125, stderr="invalid internal status")]
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results) as run,
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(
                podman_recovery.run_with_recovery(["podman", "run"], quiet_recovery=True),
                125,
            )

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [["podman", "run"], ["podman", "info"]],
        )
        self.assertIn("invalid internal status", stderr.getvalue())
        self.assertIn("stops all running containers", stderr.getvalue())
        self.assertNotIn(
            ["podman", "system", "migrate"],
            [call.args[0] for call in run.call_args_list],
        )


if __name__ == "__main__":
    unittest.main()
