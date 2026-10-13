"""Tests for shared Docker action supervision and process locking."""

from __future__ import annotations

import errno
import os
import pathlib
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from contextlib import redirect_stderr
from io import StringIO
from unittest import mock

from bazel.toolchains.cc import container_action as shared


class ContainerActionTest(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "Process-backed container simulation needs a POSIX shell")
    def test_detached_actions_stop_descendants_on_timeout_interruption_and_lost_runner(self):
        for failure in (
            "timeout",
            "interrupt",
            "terminate",
            "repeated-interrupt",
            "repeated-terminate",
            "launch-error",
            "lost-runner",
        ):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp_dir:
                root = pathlib.Path(temp_dir)
                processes = []
                tool_ready = root / "tool-ready"
                child_ready = root / "child-ready"
                tool_output = root / "tool-output"
                child_output = root / "child-output"
                unrelated_output = root / "unrelated-output"
                unrelated = subprocess.Popen(
                    [
                        "/bin/sh",
                        "-c",
                        'sleep 0.5; echo alive >"$1"',
                        "unrelated",
                        str(unrelated_output),
                    ]
                )
                command = [
                    "/bin/sh",
                    "-c",
                    'trap "" TERM; echo ready >"$1"; (echo ready >"$2"; sleep 3; echo child >"$4") & '
                    'sleep 3; echo tool >"$3"; wait',
                    "test-tool",
                    str(tool_ready),
                    str(child_ready),
                    str(tool_output),
                    str(child_output),
                ]
                interrupted = False
                cancellation_sent = False
                interrupt_failure = failure in {
                    "interrupt",
                    "terminate",
                    "repeated-interrupt",
                    "repeated-terminate",
                }
                interrupt_signal = signal.SIGTERM if "terminate" in failure else signal.SIGINT

                def fake_run(args, *, stdout=None, stderr=None, timeout=None):
                    nonlocal interrupted, cancellation_sent
                    if args[1] == "exec":
                        index = args.index("container") + 1
                        if shared.ACTION_OUTPUT_CAPTURE_SCRIPT in args:
                            environment = dict(os.environ)
                            for offset, token in enumerate(args[:index]):
                                if token == "-e":
                                    key, value = args[offset + 1].split("=", 1)
                                    environment[key] = value
                            process = subprocess.Popen(
                                args[index:],
                                env=environment,
                                start_new_session=True,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL,
                            )
                            processes.append(process)
                            deadline = time.monotonic() + 5
                            while not (tool_ready.exists() and child_ready.exists()):
                                self.assertLess(time.monotonic(), deadline)
                                time.sleep(0.01)
                            if failure == "launch-error":
                                return subprocess.CompletedProcess(args, 1, "", "connection reset")
                        else:
                            if failure.startswith("repeated-"):
                                os.kill(os.getpid(), signal.SIGINT)
                                os.kill(os.getpid(), signal.SIGTERM)
                            subprocess.run(args[index:], check=True)
                    elif args[1] == "inspect":
                        return subprocess.CompletedProcess(
                            args, 0, "container-id true started\n", ""
                        )
                    elif args[1] == "cp":
                        if args[2].startswith("container:"):
                            source = pathlib.Path(args[2].split(":", 1)[1])
                            if not source.exists():
                                if cancellation_sent and failure.startswith("repeated-"):
                                    os.kill(os.getpid(), signal.SIGINT)
                                    os.kill(os.getpid(), signal.SIGTERM)
                                if interrupt_failure and not interrupted:
                                    interrupted = True
                                    os.kill(os.getpid(), interrupt_signal)
                                return subprocess.CompletedProcess(args, 1, "", "")
                            shutil.copyfile(source, pathlib.Path(args[3]) / source.name)
                        else:
                            if args[3].endswith(shared.ACTION_CANCEL_SUFFIX):
                                if failure.startswith("repeated-"):
                                    os.kill(os.getpid(), signal.SIGINT)
                                    os.kill(os.getpid(), signal.SIGTERM)
                                cancellation_sent = True
                            shutil.copyfile(args[2], args[3].split(":", 1)[1])
                    return subprocess.CompletedProcess(args, 0, "", "")

                try:
                    with (
                        mock.patch.object(shared, "run", side_effect=fake_run),
                        mock.patch.object(
                            shared,
                            "CONTAINER_ACTION_STATE_DIR",
                            pathlib.PurePosixPath(root),
                        ),
                    ):
                        if failure == "lost-runner":
                            _, capture = shared._action_output_capture(
                                pathlib.PurePosixPath(root / "lost.log"), command
                            )
                            capture[2] = capture[2].replace(
                                "SECONDS - renewed >= 30", "SECONDS - renewed >= 1"
                            )
                            environment = dict(
                                os.environ, MONGO_CROSS_ACTION_OUTPUT_LOG=str(root / "lost.log")
                            )
                            processes.append(
                                subprocess.Popen(
                                    capture,
                                    env=environment,
                                    start_new_session=True,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL,
                                )
                            )
                            processes[-1].wait(timeout=5)
                        elif interrupt_failure:
                            exception = (
                                KeyboardInterrupt
                                if interrupt_signal == signal.SIGINT
                                else SystemExit
                            )
                            previous_handlers = {
                                signum: signal.getsignal(signum)
                                for signum in (signal.SIGINT, signal.SIGTERM)
                            }
                            with self.assertRaises(exception) as caught:
                                shared.run_container_command(
                                    ["docker"], "container", root, [], command, root
                                )
                            if exception is SystemExit:
                                self.assertEqual(caught.exception.code, 128 + signal.SIGTERM)
                            self.assertTrue(cancellation_sent)
                            for signum, handler in previous_handlers.items():
                                self.assertEqual(signal.getsignal(signum), handler)
                        else:
                            result = shared.run_container_command(
                                ["docker"], "container", root, [], command, root, timeout=0
                            )
                            self.assertEqual(result[0], 1 if failure == "launch-error" else 125)
                    for process in processes:
                        process.wait(timeout=5)
                    unrelated.wait(timeout=5)
                    time.sleep(3)
                    self.assertFalse(tool_output.exists())
                    self.assertFalse(child_output.exists())
                    self.assertEqual(unrelated_output.read_text().strip(), "alive")
                    if failure != "lost-runner":
                        self.assertEqual(list(root.glob("*.log*")), [])
                finally:
                    for process in processes:
                        if process.poll() is None:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
                    if unrelated.poll() is None:
                        unrelated.kill()
                        unrelated.wait()

    @unittest.skipIf(os.name == "nt", "Process-backed container simulation needs a POSIX shell")
    def test_action_supervisor_preserves_tool_exit_code_and_output(self):
        for status in (0, 42, 127):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as temp_dir:
                log_file = pathlib.Path(temp_dir) / "action.log"
                _, command = shared._action_output_capture(
                    pathlib.PurePosixPath(log_file),
                    [
                        "/bin/sh",
                        "-c",
                        'printf "out\\n"; printf "err\\n" >&2; exit "$1"',
                        "tool",
                        str(status),
                    ],
                )
                result = subprocess.run(
                    command,
                    env=dict(os.environ, MONGO_CROSS_ACTION_OUTPUT_LOG=str(log_file)),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0)
                self.assertEqual(log_file.read_text(), "out\nerr\n")
                self.assertEqual(
                    log_file.with_name(log_file.name + ".exit").read_text(), f"{status}\n"
                )

    def test_cancellation_waits_for_the_lease_when_docker_is_unreachable(self):
        now = 0.0

        def sleep(seconds):
            nonlocal now
            now += seconds

        with (
            tempfile.TemporaryDirectory() as temp_dir,
            mock.patch.object(shared, "run", side_effect=OSError("connection lost")),
            mock.patch.object(shared.time, "monotonic", side_effect=lambda: now),
            mock.patch.object(shared.time, "sleep", side_effect=sleep),
            redirect_stderr(StringIO()),
        ):
            shared._cancel_container_action(
                ["docker"],
                "container",
                pathlib.PurePosixPath("/tmp/action.log"),
                pathlib.Path(temp_dir),
            )
        self.assertGreaterEqual(
            now,
            shared.ACTION_LEASE_SECONDS + shared.ACTION_TERMINATION_GRACE_SECONDS,
        )

    @unittest.skipIf(os.name == "nt", "Signal delivery needs POSIX")
    def test_repeated_signals_do_not_skip_the_lease_when_docker_is_unreachable(self):
        now = 0.0

        def sleep(seconds):
            nonlocal now
            os.kill(os.getpid(), signal.SIGINT)
            os.kill(os.getpid(), signal.SIGTERM)
            now += seconds

        previous_handlers = {
            signum: signal.getsignal(signum) for signum in (signal.SIGINT, signal.SIGTERM)
        }
        with (
            tempfile.TemporaryDirectory() as directory,
            mock.patch.object(shared, "run", side_effect=OSError("connection lost")),
            mock.patch.object(shared.time, "monotonic", side_effect=lambda: now),
            mock.patch.object(shared.time, "sleep", side_effect=sleep),
            redirect_stderr(StringIO()),
        ):
            shared._cancel_container_action(
                ["docker"],
                "container",
                pathlib.PurePosixPath("/tmp/action.log"),
                pathlib.Path(directory),
            )
        self.assertGreaterEqual(
            now, shared.ACTION_LEASE_SECONDS + shared.ACTION_TERMINATION_GRACE_SECONDS
        )
        for signum, handler in previous_handlers.items():
            self.assertEqual(signal.getsignal(signum), handler)

    def test_requests_are_bounded_including_stalled_cancellation_and_cleanup(self):
        now = 0.0
        timeouts = []

        def sleep(seconds):
            nonlocal now
            now += seconds

        def stalled(args, *, stdout=None, stderr=None, timeout=None):
            nonlocal now
            self.assertIsNotNone(timeout)
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, shared.ACTION_HEARTBEAT_INTERVAL_SECONDS)
            timeouts.append(timeout)
            now += timeout
            raise subprocess.TimeoutExpired(args, timeout)

        with (
            tempfile.TemporaryDirectory() as directory,
            mock.patch.object(shared, "run", side_effect=stalled),
            mock.patch.object(shared.time, "monotonic", side_effect=lambda: now),
            mock.patch.object(shared.time, "sleep", side_effect=sleep),
            redirect_stderr(StringIO()),
        ):
            status, output, _ = shared.run_container_command(
                ["docker"],
                "container",
                pathlib.PurePosixPath("/"),
                [],
                ["/tool"],
                pathlib.Path(directory),
                timeout=0.5,
            )
        self.assertEqual(status, 125)
        self.assertIn("Docker request", output)
        self.assertGreaterEqual(now, shared.ACTION_LEASE_SECONDS)
        self.assertLess(now, shared.ACTION_LEASE_SECONDS + 5)
        self.assertGreater(len(timeouts), 2)

    def test_stalled_polling_cancels_action_and_cleanup_does_not_mask_error(self):
        for stalled_request in ("inspect", "exit", "heartbeat"):
            with self.subTest(request=stalled_request), tempfile.TemporaryDirectory() as directory:

                def fake_run(args, *, stdout=None, stderr=None, timeout=None):
                    self.assertGreater(timeout, 0)
                    self.assertLessEqual(timeout, 5)
                    is_exit = "cp" in args and args[2].endswith(".exit")
                    is_heartbeat = "cp" in args and args[3].endswith(".heartbeat")
                    if (
                        args[1] == stalled_request
                        or (stalled_request == "exit" and is_exit)
                        or (stalled_request == "heartbeat" and is_heartbeat)
                        or "/bin/rm" in args
                    ):
                        raise subprocess.TimeoutExpired(args, timeout)
                    return subprocess.CompletedProcess(
                        args, 1 if "cp" in args else 0, "id true started", ""
                    )

                with (
                    mock.patch.object(shared, "run", side_effect=fake_run),
                    mock.patch.object(shared, "_cancel_container_action") as cancel,
                ):
                    status, output, _ = shared.run_container_command(
                        ["docker"],
                        "container",
                        pathlib.PurePosixPath("/"),
                        [],
                        ["/tool"],
                        pathlib.Path(directory),
                        timeout=10,
                    )
                self.assertEqual(status, 125)
                self.assertIn("Docker request", output)
                cancel.assert_called_once()

    def test_output_copy_retries_and_retains_unrecoverable_logs(self):
        for failures in (0, 1, 100):
            for output in ("", "compiler diagnostic\n"):
                with (
                    self.subTest(failures=failures, output=output),
                    tempfile.TemporaryDirectory() as directory,
                ):
                    runtime, calls, files = self._fake_container_runtime({"/tool": (output, 0)})
                    remaining_failures = failures
                    now = 0.0

                    def sleep(seconds):
                        nonlocal now
                        now += seconds

                    def flaky(args, **kwargs):
                        nonlocal remaining_failures
                        if "cp" in args and args[2].endswith(".log") and remaining_failures:
                            remaining_failures -= 1
                            return subprocess.CompletedProcess(args, 1, "", "connection reset")
                        return runtime(args, **kwargs)

                    with (
                        mock.patch.object(shared, "run", side_effect=flaky),
                        mock.patch.object(shared.time, "monotonic", side_effect=lambda: now),
                        mock.patch.object(shared.time, "sleep", side_effect=sleep),
                    ):
                        status, actual_output, _ = shared.run_container_command(
                            ["docker"],
                            "container",
                            pathlib.PurePosixPath("/"),
                            [],
                            ["/tool"],
                            pathlib.Path(directory),
                        )
                    if failures == 100:
                        self.assertEqual(status, 125)
                        self.assertIn("log retained at container:", actual_output)
                        self.assertFalse(any("/bin/rm" in call for call in calls))
                        self.assertEqual(now, shared.ACTION_LOG_TIMEOUT_SECONDS)
                    else:
                        self.assertEqual(status, 0)
                        self.assertEqual(actual_output, output)
                        self.assertTrue(any("/bin/rm" in call for call in calls))

    def test_helper_start_inspect_logs_and_cleanup_use_deadlines(self):
        for failure in ("run", "inspect", "logs", "rm"):
            calls = []

            def fake_run(args, *, stdout=None, stderr=None, timeout=None):
                self.assertIsNotNone(timeout)
                self.assertGreater(timeout, 0)
                self.assertLessEqual(timeout, 0.5 if args[1] != "rm" else 5)
                calls.append(args[1])
                if args[1] == failure:
                    raise subprocess.TimeoutExpired(args, timeout)
                return subprocess.CompletedProcess(args, 0, "exited 0", "")

            with mock.patch.object(shared, "run", side_effect=fake_run):
                status, output = shared._run_helper_container(
                    ["docker"], "image", ["/tool"], timeout=0.5
                )
            self.assertIn("rm", calls)
            self.assertEqual(status, 0 if failure == "rm" else 125)

    def test_windows_lock_fallback_does_not_require_fcntl(self):
        class Msvcrt:
            LK_LOCK = 1
            LK_UNLCK = 2

            calls: list[tuple[int, int, int]] = []

            @classmethod
            def locking(cls, descriptor: int, mode: int, size: int) -> None:
                cls.calls.append((descriptor, mode, size))

        with tempfile.TemporaryDirectory() as temp_dir:
            lock_path = pathlib.Path(temp_dir) / "container.lock"
            with (
                mock.patch.object(shared, "fcntl", None),
                mock.patch.object(shared, "msvcrt", Msvcrt),
                shared.exclusive_file_lock(lock_path),
            ):
                self.assertTrue(lock_path.exists())

        self.assertEqual([mode for _, mode, _ in Msvcrt.calls], [Msvcrt.LK_LOCK, Msvcrt.LK_UNLCK])

    def test_action_output_capture_records_status_next_to_the_container_log(self):
        log_file = pathlib.PurePosixPath("/tmp/mongo-cross-actions/1-abc.log")
        environment, command = shared._action_output_capture(
            log_file, ["/z/b/b/external/toolchain/llvm/bin/lld", "-flavor", "link", "@params"]
        )
        self.assertEqual(
            environment,
            ["-e", "MONGO_CROSS_ACTION_OUTPUT_LOG=/tmp/mongo-cross-actions/1-abc.log"],
        )
        self.assertEqual(
            command,
            [
                "/bin/bash",
                "-c",
                shared.ACTION_OUTPUT_CAPTURE_SCRIPT,
                "/z/b/b/external/toolchain/llvm/bin/lld",
                "-flavor",
                "link",
                "@params",
            ],
        )

    @staticmethod
    def _fake_container_runtime(records: dict[str, tuple[str, int]]):
        """Simulates ``docker exec -d`` plus ``docker cp`` against an in-memory container.

        ``records`` maps a command's first argument to the (output, status) the
        container should produce for it. Files become copyable only after the
        command "ran".
        """
        container_files: dict[str, str] = {}
        calls: list[list[str]] = []
        capture_script = shared.ACTION_OUTPUT_CAPTURE_SCRIPT

        def fake_run(args, *, stdout=None, stderr=None, timeout=None):
            calls.append(list(args))
            if "info" in args:
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout="linux\n", stderr=""
                )
            if "inspect" in args:
                return subprocess.CompletedProcess(args, 0, "container-id true started\n", "")
            if "exec" in args and "-d" in args and capture_script in args:
                command = args[args.index(capture_script) + 1 :]
                log_env = next(token for token in args if "MONGO_CROSS_ACTION_OUTPUT_LOG=" in token)
                log_path = log_env.split("=", 1)[1]
                output, status = records.get(command[0], ("", 0))
                container_files[log_path] = output
                container_files[f"{log_path}.exit"] = f"{status}\n"
                return subprocess.CompletedProcess(args=args, returncode=0, stdout="", stderr="")
            if "cp" in args:
                source = args[args.index("cp") + 1].split(":", 1)[1]
                destination = pathlib.Path(args[args.index("cp") + 2])
                if source not in container_files:
                    return subprocess.CompletedProcess(
                        args=args, returncode=1, stdout="", stderr="Could not find the file\n"
                    )
                (destination / pathlib.PurePosixPath(source).name).write_text(
                    container_files[source], encoding="utf-8"
                )
                return subprocess.CompletedProcess(args=args, returncode=0, stdout="", stderr="")
            return subprocess.CompletedProcess(args=args, returncode=0, stdout="", stderr="")

        return fake_run, calls, container_files

    def test_run_container_command_takes_status_and_output_through_docker_cp(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir).resolve()
            fake_run, calls, _ = self._fake_container_runtime(
                {"/z/toolchain/llvm/bin/lld": ("lld: error while loading shared libraries\n", 127)}
            )

            with mock.patch.object(shared, "run", side_effect=fake_run):
                exit_code, output, launch = shared.run_container_command(
                    ["docker"],
                    "container",
                    pathlib.Path("/mongo-execroot"),
                    ["-e", "LIB=/z/lib"],
                    ["/z/toolchain/llvm/bin/lld", "-flavor", "link", "@params"],
                    repo_root,
                )

            self.assertEqual(exit_code, 127)
            self.assertEqual(output, "lld: error while loading shared libraries\n")
            self.assertEqual(launch, calls[0])
            self.assertEqual(launch[:6], ["docker", "exec", "-d", "-w", "/mongo-execroot", "-e"])
            self.assertEqual(launch[6], "LIB=/z/lib")
            self.assertEqual(
                launch[-4:], ["/z/toolchain/llvm/bin/lld", "-flavor", "link", "@params"]
            )
            self.assertTrue(
                any(
                    "cp" in call
                    and call[call.index("cp") + 1].startswith("container:/tmp/mongo-cross-actions/")
                    for call in calls
                )
            )
            # The host scratch directory is removed again.
            log_dir = repo_root
            self.assertEqual(list(log_dir.iterdir()), [])

    def test_run_container_command_reports_exec_start_failures_and_timeouts(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir).resolve()

            with mock.patch.object(
                shared,
                "run",
                return_value=subprocess.CompletedProcess(
                    args=[], returncode=1, stdout="", stderr="Error: No such container: x\n"
                ),
            ):
                with mock.patch.object(shared, "_cancel_container_action") as cancel:
                    exit_code, output, _ = shared.run_container_command(
                        ["docker"], "x", pathlib.Path("/"), [], ["/tool"], repo_root
                    )
                cancel.assert_called_once()
            self.assertEqual(exit_code, 1)
            self.assertIn("No such container", output)

            def never_completes(args, *, stdout=None, stderr=None, timeout=None):
                # exec -d succeeds, but docker cp never finds the status file.
                if "cp" in args:
                    return subprocess.CompletedProcess(
                        args=args, returncode=1, stdout="", stderr=""
                    )
                if "inspect" in args:
                    return subprocess.CompletedProcess(args, 0, "container-id true started\n", "")
                return subprocess.CompletedProcess(args=args, returncode=0, stdout="", stderr="")

            with mock.patch.object(shared, "run", side_effect=never_completes):
                with mock.patch.object(shared, "_cancel_container_action") as cancel:
                    exit_code, output, _ = shared.run_container_command(
                        ["docker"], "x", pathlib.Path("/"), [], ["/tool"], repo_root, timeout=0.0
                    )
                cancel.assert_called_once()
            self.assertEqual(exit_code, 125)
            self.assertIn("did not report completion", output)

    def test_mount_windows_drives_in_docker_vm_uses_a_privileged_helper(self):
        calls: list[list[str]] = []

        def fake_run(args, *, stdout=None, stderr=None, timeout=None):
            calls.append(list(args))
            if "run" in args:
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout="abc123\n", stderr=""
                )
            if "inspect" in args:
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout="exited 0\n", stderr=""
                )
            if "logs" in args:
                return subprocess.CompletedProcess(
                    args=args,
                    returncode=0,
                    stdout="Z: /mnt/z 9p rw 0 0\nC: /mnt/c 9p rw 0 0\n",
                    stderr="",
                )
            return subprocess.CompletedProcess(args=args, returncode=0, stdout="", stderr="")

        with mock.patch.object(shared, "run", side_effect=fake_run):
            mounted, detail = shared.mount_windows_drives_in_docker_vm(
                ["docker"], "example/image", ["c", "z"], "/mnt"
            )

        self.assertTrue(mounted, detail)
        run_call = next(call for call in calls if "run" in call)
        self.assertEqual(run_call[:3], ["docker", "run", "-d"])
        self.assertIn("--privileged", run_call)
        self.assertIn("--pid=host", run_call)
        image_index = run_call.index("example/image")
        self.assertEqual(
            run_call[image_index + 1 : image_index + 7], ["nsenter", "-t", "1", "-m", "--", "sh"]
        )
        script = run_call[-1]
        self.assertIn("mount -t drvfs C: /mnt/c", script)
        self.assertIn("mount -t drvfs Z: /mnt/z", script)
        self.assertIn("mountpoint -q /mnt/z", script)
        # The helper container is always removed again.
        self.assertTrue(any("rm" in call and "-f" in call for call in calls))

    def test_mount_windows_drives_in_docker_vm_reports_failures(self):
        def fake_run(args, *, stdout=None, stderr=None, timeout=None):
            if "run" in args:
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout="abc123\n", stderr=""
                )
            if "inspect" in args:
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout="exited 0\n", stderr=""
                )
            if "logs" in args:
                return subprocess.CompletedProcess(
                    args=args,
                    returncode=0,
                    stdout="mount: unknown filesystem type 'drvfs'\nmount of Z: at /mnt/z failed\n",
                    stderr="",
                )
            return subprocess.CompletedProcess(args=args, returncode=0, stdout="", stderr="")

        with mock.patch.object(shared, "run", side_effect=fake_run):
            mounted, detail = shared.mount_windows_drives_in_docker_vm(
                ["docker"], "example/image", ["z"], "/mnt"
            )
        self.assertFalse(mounted)
        self.assertIn("unknown filesystem type", detail)

    def test_run_container_command_stops_when_the_container_is_replaced(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir).resolve()
            identities = iter(["sha256:old", "sha256:new"])

            def fake_run(args, *, stdout=None, stderr=None, timeout=None):
                if "inspect" in args:
                    return subprocess.CompletedProcess(
                        args=args,
                        returncode=0,
                        stdout=next(identities, "sha256:new") + " true started\n",
                        stderr="",
                    )
                if "cp" in args:
                    return subprocess.CompletedProcess(
                        args=args, returncode=1, stdout="", stderr=""
                    )
                return subprocess.CompletedProcess(args=args, returncode=0, stdout="", stderr="")

            with (
                mock.patch.object(shared, "run", side_effect=fake_run),
                mock.patch.object(shared, "ACTION_COMPLETION_POLL_SECONDS", 0.0),
            ):
                exit_code, output, _ = shared.run_container_command(
                    ["docker"], "x", pathlib.Path("/"), [], ["/tool"], repo_root, timeout=30.0
                )
        self.assertEqual(exit_code, 125)
        self.assertIn("was replaced", output)

    def test_stopped_and_restarted_containers_fail_without_waiting_for_action_timeout(self):
        for state, detail in (
            ("container-id false started", "stopped"),
            ("container-id true restarted", "restarted"),
        ):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as directory:
                now = 0.0
                states = iter(["container-id true started", state])

                def sleep(seconds):
                    nonlocal now
                    now += seconds
                    self.assertLess(now, 5, "dead actions must fail promptly")

                def fake_run(args, **kwargs):
                    if args[1] == "inspect":
                        return subprocess.CompletedProcess(args, 0, next(states, state), "")
                    return subprocess.CompletedProcess(args, 1 if args[1] == "cp" else 0, "", "")

                with (
                    mock.patch.object(shared, "run", side_effect=fake_run),
                    mock.patch.object(shared.time, "monotonic", side_effect=lambda: now),
                    mock.patch.object(shared.time, "sleep", side_effect=sleep),
                    mock.patch.object(shared, "_cancel_container_action") as cancel,
                ):
                    status, output, _ = shared.run_container_command(
                        ["docker"],
                        "container",
                        pathlib.PurePosixPath("/"),
                        [],
                        ["/tool"],
                        pathlib.Path(directory),
                    )
                self.assertEqual(status, 125)
                self.assertIn(detail, output)
                cancel.assert_not_called()

    def test_read_exit_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            exit_file = pathlib.Path(temp_dir) / "action.log.exit"
            self.assertIsNone(shared._read_exit_file(exit_file))
            exit_file.write_text("", encoding="utf-8")
            self.assertIsNone(shared._read_exit_file(exit_file))
            exit_file.write_text("3\n", encoding="utf-8")
            self.assertEqual(shared._read_exit_file(exit_file), 3)
            exit_file.write_text("garbage\n", encoding="utf-8")
            self.assertEqual(shared._read_exit_file(exit_file), 125)

    def test_read_and_remove_output_log(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            log_file = pathlib.Path(temp_dir) / "action.log"
            self.assertIsNone(shared._read_and_remove_output_log(log_file))
            log_file.write_text("ld.so: missing library\n", encoding="utf-8")
            self.assertEqual(
                shared._read_and_remove_output_log(log_file),
                "ld.so: missing library\n",
            )
            self.assertFalse(log_file.exists())

    def test_windows_lock_retries_when_another_action_is_starting(self):
        class Msvcrt:
            LK_NBLCK = 3
            LK_UNLCK = 2
            attempts = 0
            calls: list[tuple[int, int, int]] = []

            @classmethod
            def locking(cls, descriptor: int, mode: int, size: int) -> None:
                cls.calls.append((descriptor, mode, size))
                if mode == cls.LK_NBLCK and cls.attempts == 0:
                    cls.attempts += 1
                    raise OSError(errno.EDEADLK, "Resource deadlock avoided")

        with tempfile.TemporaryDirectory() as temp_dir:
            lock_path = pathlib.Path(temp_dir) / "container.lock"
            with (
                mock.patch.object(shared, "fcntl", None),
                mock.patch.object(shared, "msvcrt", Msvcrt),
                mock.patch.object(shared.time, "sleep"),
                shared.exclusive_file_lock(lock_path),
            ):
                pass

        self.assertEqual(
            [mode for _, mode, _ in Msvcrt.calls],
            [Msvcrt.LK_NBLCK] * 2 + [Msvcrt.LK_UNLCK],
        )


if __name__ == "__main__":
    unittest.main()
