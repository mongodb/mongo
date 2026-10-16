"""Tests for the shared persistent-container protocol and preflight diagnostics."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from unittest import mock

from bazel.toolchains.cc import container_action
from bazel.wrapper_hook.hermetic_container import container_actions


class ContainerActionsTest(unittest.TestCase):
    def test_config_lock_requires_a_locking_api(self):
        with tempfile.TemporaryDirectory() as temporary:
            with (
                mock.patch.object(container_action, "fcntl", None),
                mock.patch.object(container_action, "msvcrt", None),
                self.assertRaisesRegex(RuntimeError, "no file locking primitive"),
                container_actions.lock(pathlib.Path(temporary) / "output-base"),
            ):
                self.fail("Config publication must not proceed without locking")

    def test_config_lock_serializes_other_processes(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_base = pathlib.Path(temporary) / "output-base"
            script = (
                "import sys; from bazel.wrapper_hook.hermetic_container import container_actions; "
                "from pathlib import Path; print('ready', flush=True); "
                "\nwith container_actions.lock(Path(sys.argv[1])): print('acquired', flush=True)"
            )
            process = None
            try:
                with container_actions.lock(output_base):
                    process = subprocess.Popen(
                        [container_actions.sys.executable, "-c", script, str(output_base)],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                    )
                    self.assertEqual(process.stdout.readline(), "ready\n")
                    with self.assertRaises(subprocess.TimeoutExpired):
                        process.communicate(timeout=0.1)
                output, error = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, error)
                self.assertEqual(output, "acquired\n")
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_protocol_keeps_runner_and_output_paths_together(self):
        output_base = pathlib.Path("/output base")
        runner = pathlib.Path("/runner path/windows.py")
        sandbox = pathlib.Path("/sandbox base")
        with mock.patch.object(
            container_actions, "_linux_action_sandbox_base", return_value=sandbox
        ):
            options = container_actions.persistent_container_options(output_base, runner)
        self.assertEqual(
            options,
            [
                "--experimental_enable_persistent_container_sandbox",
                f"--experimental_persistent_container_python={container_actions.sys.executable}",
                f"--experimental_persistent_container_runner={runner}",
                f"--experimental_persistent_container_config={output_base / container_actions.CONTAINER_ACTIONS_CONFIG_FILENAME}",
                f"--experimental_persistent_container_worker_dir={sandbox / 'persistent-workers'}",
                f"--sandbox_base={sandbox}",
            ],
        )

    def test_preflight_preserves_platform_output_policy(self):
        for report_output, expected in [
            (False, "mount warning"),
            (True, "mount warning\nprobe passed"),
        ]:
            with self.subTest(report_output=report_output):
                output = StringIO()
                result = subprocess.CompletedProcess([], 0, "probe passed\n", "mount warning\n")
                with (
                    mock.patch.object(
                        container_actions.subprocess, "run", return_value=result
                    ) as run,
                    mock.patch.object(
                        container_actions,
                        "_compact_runtime_detail",
                        side_effect=lambda x: x.strip(),
                    ),
                    redirect_stderr(output),
                ):
                    ready, detail = container_actions.preflight_container_runner(
                        pathlib.Path("/runner.py"),
                        pathlib.Path("/config.json"),
                        timeout=180,
                        report_output=report_output,
                    )
                self.assertTrue(ready)
                self.assertEqual(detail, expected)
                self.assertEqual(output.getvalue(), expected + "\n" if report_output else "")
                self.assertEqual(
                    run.call_args.args[0][-2:],
                    ["--ensure-container", str(pathlib.Path("/config.json"))],
                )
                self.assertEqual(run.call_args.kwargs["timeout"], 180)
                self.assertEqual(
                    run.call_args.kwargs.get("errors"), "replace" if report_output else None
                )

    def test_preflight_timeout_preserves_platform_description(self):
        with mock.patch.object(
            container_actions.subprocess, "run", side_effect=subprocess.TimeoutExpired([], 30)
        ):
            self.assertEqual(
                container_actions.preflight_container_runner(
                    pathlib.Path("/runner.py"),
                    pathlib.Path("/config.json"),
                    timeout=30,
                    timeout_description="container start/exec preflight",
                ),
                (False, "container start/exec preflight timed out after 30s"),
            )


if __name__ == "__main__":
    unittest.main()
