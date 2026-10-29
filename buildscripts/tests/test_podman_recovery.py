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

    def test_idle_stale_podman_runtime_is_migrated(self) -> None:
        stale = (
            'invalid internal status, try resetting the pause process with "podman system migrate"'
        )
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
            _result(["podman", "ps", "--quiet"], 0, stdout="\n"),
            _result(["podman", "system", "migrate"], 0),
            _result(["podman", "info"], 0),
            _result(["podman", "run"], 0),
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
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 0)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                ["podman", "run"],
                ["podman", "info"],
                ["podman", "ps", "--quiet"],
                ["podman", "system", "migrate"],
                ["podman", "info"],
                ["podman", "run"],
            ],
        )
        self.assertIn("Checking whether another process has recovered", stderr.getvalue())

    def test_unreachable_containers_block_migration_without_force(self) -> None:
        stale = "invalid internal status"
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
            _result(["podman", "ps", "--quiet"], 125, stderr=stale),
        ]
        stderr = io.StringIO()
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results) as run,
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 125)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [["podman", "run"], ["podman", "info"], ["podman", "ps", "--quiet"]],
        )
        self.assertIn(
            "could not determine whether this user has running containers",
            stderr.getvalue(),
        )

    def test_unreachable_containers_can_be_force_migrated(self) -> None:
        stale = "invalid internal status"
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
            _result(["podman", "ps", "--quiet"], 125, stderr=stale),
            _result(["podman", "system", "migrate"], 0),
            _result(["podman", "info"], 0),
            _result(["podman", "run"], 0),
        ]
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results) as run,
            mock.patch.dict(
                podman_recovery.os.environ,
                {"MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE": "1"},
            ),
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 0)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                ["podman", "run"],
                ["podman", "info"],
                ["podman", "ps", "--quiet"],
                ["podman", "system", "migrate"],
                ["podman", "info"],
                ["podman", "run"],
            ],
        )

    def test_running_containers_block_migration(self) -> None:
        stale = "invalid internal status"
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
            _result(["podman", "ps", "--quiet"], 0, stdout="deadbeef\n"),
        ]
        stderr = io.StringIO()
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results) as run,
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 125)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [["podman", "run"], ["podman", "info"], ["podman", "ps", "--quiet"]],
        )
        self.assertIn("stops the containers currently running", stderr.getvalue())

    def test_migration_can_be_disabled_by_environment(self) -> None:
        stale = "invalid internal status"
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
        ]
        stderr = io.StringIO()
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results) as run,
            mock.patch.dict(
                podman_recovery.os.environ,
                {"MONGO_BAZEL_PODMAN_AUTO_MIGRATE": "0"},
            ),
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 125)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [["podman", "run"], ["podman", "info"]],
        )
        self.assertIn("MONGO_BAZEL_PODMAN_AUTO_MIGRATE", stderr.getvalue())

    def test_failed_migration_reports_original_failure(self) -> None:
        stale = "invalid internal status"
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
            _result(["podman", "ps", "--quiet"], 0),
            _result(["podman", "system", "migrate"], 1, stderr="migrate blew up"),
        ]
        stderr = io.StringIO()
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results),
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 125)

        self.assertIn("migrate blew up", stderr.getvalue())
        self.assertIn("`podman system migrate` failed", stderr.getvalue())

    def test_migration_rechecks_runtime_before_retrying(self) -> None:
        stale = "invalid internal status"
        results = [
            _result(["podman", "run"], 125, stderr=stale),
            _result(["podman", "info"], 125, stderr=stale),
            _result(["podman", "ps", "--quiet"], 0),
            _result(["podman", "system", "migrate"], 0),
            _result(["podman", "info"], 125, stderr=stale),
        ]
        stderr = io.StringIO()
        with (
            mock.patch.object(podman_recovery.subprocess, "run", side_effect=results) as run,
            mock.patch.object(
                podman_recovery,
                "_recovery_lock",
                return_value=contextlib.nullcontext(),
            ),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(podman_recovery.run_with_recovery(["podman", "run"]), 125)

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                ["podman", "run"],
                ["podman", "info"],
                ["podman", "ps", "--quiet"],
                ["podman", "system", "migrate"],
                ["podman", "info"],
            ],
        )
        self.assertIn("remained unavailable after `podman system migrate`", stderr.getvalue())

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

    def test_quiet_recovery_reports_stale_runtime_when_containers_are_running(self) -> None:
        stale = _result(["podman", "run"], 125, stderr="invalid internal status")
        results = [
            stale,
            _result(["podman", "info"], 125, stderr="invalid internal status"),
            _result(["podman", "ps", "--quiet"], 0, stdout="deadbeef\n"),
        ]
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
            [["podman", "run"], ["podman", "info"], ["podman", "ps", "--quiet"]],
        )
        self.assertIn("invalid internal status", stderr.getvalue())
        self.assertIn("stops the containers currently running", stderr.getvalue())
        self.assertNotIn(
            ["podman", "system", "migrate"],
            [call.args[0] for call in run.call_args_list],
        )


if __name__ == "__main__":
    unittest.main()
