import os
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from buildscripts import prepare_podman_for_bazel
from buildscripts.package_test import package_test_container as under_test


class DockerRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.client = mock.Mock()
        self.runtime = under_test.DockerRuntime(self.client)

    def test_run_preserves_command_mounts_and_environment(self):
        command = ["bash", "-c", 'printf "%s" "$VALUE"']
        volumes = ["/path with spaces:/mnt/package_test", "/openssl:/openssl:ro"]
        environment = {"MONGO_RELEASE_BINARY_HASHES": '{"mongod": "hash"}'}
        container = self.runtime.run("almalinux:9", command, volumes, environment)
        self.client.containers.run.assert_called_once_with(
            "almalinux:9",
            command=command,
            detach=True,
            user="root",
            volumes=volumes,
            environment=environment,
        )
        self.client.containers.run.return_value.logs.return_value = [b"log\n"]
        self.client.containers.run.return_value.wait.return_value = {"StatusCode": 37}
        self.assertEqual([b"log\n"], list(container.logs()))
        self.assertEqual(37, container.wait())
        container.remove()
        self.client.containers.run.return_value.remove.assert_called_once_with(force=True)

    def test_pull_returns_image_reference(self):
        self.assertEqual("almalinux:9", self.runtime.pull("almalinux:9"))
        self.client.images.pull.assert_called_once_with("almalinux:9")

    def test_missing_image_uses_runtime_independent_error(self):
        from docker.errors import ImageNotFound

        self.client.images.pull.side_effect = ImageNotFound("missing")
        with self.assertRaises(under_test.ContainerError):
            self.runtime.pull("missing")

    def test_api_errors_use_runtime_independent_error(self):
        from docker.errors import APIError

        self.client.containers.run.side_effect = APIError("cannot create container")
        with self.assertRaisesRegex(under_test.ContainerError, "cannot create container"):
            self.runtime.run("almalinux:9", ["true"], [], {})


class PodmanRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.run = self.enterContext(mock.patch.object(under_test.subprocess, "run"))
        self.run.return_value = subprocess.CompletedProcess([], 0, "container-id\n", "")
        self.runtime = under_test.PodmanRuntime()
        self.addCleanup(self.runtime.close)

    def test_pull_qualifies_short_image_names(self):
        for image, reference in (
            ("almalinux:9", "docker.io/library/almalinux:9"),
            ("opensuse/leap:15", "docker.io/opensuse/leap:15"),
            ("registry.suse.com/suse/sle15:15.5", "registry.suse.com/suse/sle15:15.5"),
        ):
            with self.subTest(image=image):
                self.assertEqual(reference, self.runtime.pull(image))
                self.assertEqual(["podman", "pull", reference], self.run.call_args.args[0])

    def test_run_passes_shell_program_as_one_argument(self):
        command = ["bash", "-c", 'printf "%s" "$VALUE" && false']
        volumes = ["/path with spaces:/mnt/package_test", "/openssl:/openssl:ro"]
        environment = {"MONGO_RELEASE_BINARY_HASHES": '{"mongod": "hash"}'}
        self.runtime.run("docker.io/library/almalinux:9", command, volumes, environment)
        self.assertEqual(
            [
                "podman",
                "run",
                "--detach",
                "--user=root",
                "--pull=never",
                "--timeout=600",
                "--security-opt=label=disable",
                "--volume",
                volumes[0],
                "--volume",
                volumes[1],
                "--env",
                'MONGO_RELEASE_BINARY_HASHES={"mongod": "hash"}',
                "docker.io/library/almalinux:9",
                *command,
            ],
            self.run.call_args.args[0],
        )
        self.assertNotIn("shell", self.run.call_args.kwargs)

    def test_task_label_includes_evergreen_execution(self):
        with mock.patch.dict(under_test.os.environ, {"task_id": "task", "execution": "3"}):
            runtime = under_test.PodmanRuntime()
            runtime.run("almalinux:9", ["true"], [], {})
        arguments = self.run.call_args.args[0]
        self.assertIn("org.mongodb.package-test.task=task-3", arguments)

    @unittest.skipUnless(
        sys.platform == "linux", "Rootless Podman and Unix permissions require Linux"
    )
    def test_task_state_tracks_use_and_keeps_credentials_private(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            mock.patch.dict(
                under_test.os.environ,
                {
                    "task_id": "task",
                    "execution": "3",
                    "MONGO_PACKAGE_TEST_RUNTIME": "podman",
                    "MONGO_PACKAGE_TEST_TASK_ID": "task-3",
                    "MONGO_PACKAGE_TEST_STATE_DIR": str(Path(directory) / "task-3"),
                },
            ),
        ):
            state = Path(directory) / "task-3"
            prepare_podman_for_bazel.prepare_podman_for_package_tests(
                runner=self.run, which=mock.Mock(return_value="/usr/bin/podman")
            )
            runtime = under_test.PodmanRuntime()
            self.addCleanup(runtime.close)
            self.assertEqual("task-3\n", (state / "active").read_text())
            self.assertEqual(0o700, state.stat().st_mode & 0o777)
            expected_environment = runtime.runtime_environment
            self.assertEqual(
                (state / "CONTAINERS_STORAGE_CONF").read_text(),
                expected_environment["CONTAINERS_STORAGE_CONF"],
            )
            runtime.login("username", "fixture")
            self.assertEqual(expected_environment, self.run.call_args.kwargs["env"])
            first_auth_directory = Path(runtime.auth_directory.name)
            self.assertEqual(state / "credentials", first_auth_directory.parent)
            self.assertEqual(0o700, first_auth_directory.stat().st_mode & 0o777)
            runtime.login("username", "fixture")
            self.assertFalse(first_auth_directory.exists())
            second_auth_directory = Path(runtime.auth_directory.name)
            runtime.close()
            self.assertFalse(second_auth_directory.exists())
            # Post teardown must still find leftovers if per-container removal failed.
            self.assertTrue((state / "active").exists())
            runtime.pull("almalinux:9")
            self.assertEqual(expected_environment, self.run.call_args.kwargs["env"])
            container = runtime.run("almalinux:9", ["true"], [], {})
            self.assertEqual(expected_environment, self.run.call_args.kwargs["env"])
            self.run.return_value.stdout = "0\n"
            self.assertEqual(0, container.wait())
            self.assertEqual(expected_environment, self.run.call_args.kwargs["env"])
            container.remove()
            self.assertEqual(expected_environment, self.run.call_args.kwargs["env"])
            with (
                mock.patch.object(under_test, "_podman", return_value="stopped") as cli,
                mock.patch.object(under_test.subprocess, "Popen") as popen,
            ):
                process = popen.return_value.__enter__.return_value
                process.communicate.return_value = (b"log\n", None)
                process.returncode = 0
                self.assertEqual([b"log\n"], list(container.logs()))
                self.assertEqual(expected_environment, cli.call_args.kwargs["env"])
                self.assertEqual(expected_environment, popen.call_args.kwargs["env"])

    def test_task_runtime_requires_preparation_before_any_podman_command(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            mock.patch.dict(
                os.environ, {"task_id": "task", "MONGO_PACKAGE_TEST_STATE_DIR": directory}
            ),
        ):
            self.run.reset_mock()
            with self.assertRaisesRegex(under_test.ContainerError, "preparation has not completed"):
                under_test.PodmanRuntime()
            self.run.assert_not_called()

    def test_cli_timeout_reports_runtime_error(self):
        self.run.side_effect = subprocess.TimeoutExpired(["podman", "info"], 300)
        with self.assertRaisesRegex(under_test.ContainerError, "timed out"):
            under_test.PodmanRuntime()

    def test_login_uses_stdin_and_private_auth_file(self):
        self.runtime.login("username", "secret-password")
        arguments = self.run.call_args.args[0]
        self.assertEqual("secret-password", self.run.call_args.kwargs["input"])
        self.assertNotIn("secret-password", arguments)
        self.assertIn("--password-stdin", arguments)
        auth_file = arguments[arguments.index("--authfile") + 1]
        self.runtime.pull("almalinux:9")
        self.assertEqual(
            ["podman", "pull", "--authfile", auth_file, "docker.io/library/almalinux:9"],
            self.run.call_args.args[0],
        )

    def test_wait_returns_container_failure_code(self):
        container = self.runtime.run("almalinux:9", ["false"], [], {})
        self.run.return_value.stdout = "37\n"
        self.assertEqual(37, container.wait())
        self.assertEqual(["podman", "wait", "container-id"], self.run.call_args.args[0])
        container.remove()
        self.assertEqual(
            ["podman", "rm", "--force", "--time=0", "container-id"], self.run.call_args.args[0]
        )
        self.assertEqual(30, self.run.call_args.kwargs["timeout"])

    def test_streams_container_logs_as_bytes(self):
        container = self.runtime.run("almalinux:9", ["true"], [], {})
        with (
            mock.patch.object(under_test, "_podman", return_value="stopped"),
            mock.patch.object(under_test.subprocess, "Popen") as popen,
        ):
            process = popen.return_value.__enter__.return_value
            process.communicate.return_value = (b"first\nsecond\n", None)
            process.returncode = 0
            self.assertEqual([b"first\n", b"second\n"], list(container.logs()))
            self.assertEqual(["podman", "logs", "container-id"], popen.call_args.args[0])

    def test_log_snapshots_refresh_state_and_preserve_partial_lines(self):
        container = self.runtime.run("almalinux:9", ["true"], [], {})
        snapshots = [b"first\nsec", b"first\nsecond\nfinal"]
        with (
            mock.patch.object(under_test, "_podman", side_effect=["running", "stopped"]) as cli,
            mock.patch.object(under_test.subprocess, "Popen") as popen,
            mock.patch.object(under_test.time, "sleep"),
        ):
            processes = [mock.Mock(returncode=0) for snapshot in snapshots]
            for process, snapshot in zip(processes, snapshots):
                process.communicate.return_value = (snapshot, None)
            popen.return_value.__enter__.side_effect = processes
            self.assertEqual([b"first\n", b"second\n", b"final"], list(container.logs()))
            self.assertEqual(
                [mock.call(["inspect", "--format", "{{.State.Status}}", "container-id"], env=None)]
                * 2,
                cli.call_args_list,
            )

    def test_log_snapshots_reject_truncation(self):
        container = self.runtime.run("almalinux:9", ["true"], [], {})
        snapshots = [b"first\n", b"new\n"]
        with (
            mock.patch.object(under_test, "_podman", side_effect=["running", "stopped"]),
            mock.patch.object(under_test.subprocess, "Popen") as popen,
            mock.patch.object(under_test.time, "sleep"),
        ):
            processes = [mock.Mock(returncode=0) for snapshot in snapshots]
            for process, snapshot in zip(processes, snapshots):
                process.communicate.return_value = (snapshot, None)
            popen.return_value.__enter__.side_effect = processes
            with self.assertRaisesRegex(under_test.ContainerError, "truncated"):
                list(container.logs())

    def test_cli_failure_reports_diagnostics(self):
        self.run.side_effect = subprocess.CalledProcessError(
            125, ["podman", "run"], stderr="permission denied"
        )
        with self.assertRaisesRegex(under_test.ContainerError, "permission denied"):
            self.runtime.run("almalinux:9", ["true"], [], {})

    def test_failed_log_stream_reports_runtime_error(self):
        container = self.runtime.run("almalinux:9", ["true"], [], {})
        with (
            mock.patch.object(under_test, "_podman", return_value="stopped"),
            mock.patch.object(under_test.subprocess, "Popen") as popen,
        ):
            process = popen.return_value.__enter__.return_value
            process.communicate.return_value = (b"error\n", None)
            process.returncode = 125
            with self.assertRaisesRegex(under_test.ContainerError, "podman logs failed"):
                list(container.logs())

    def test_hung_log_command_is_killed(self):
        container = self.runtime.run("almalinux:9", ["true"], [], {})
        with (
            mock.patch.object(under_test, "_podman", return_value="running"),
            mock.patch.object(under_test.subprocess, "Popen") as popen,
        ):
            process = popen.return_value.__enter__.return_value
            process.communicate.side_effect = [
                subprocess.TimeoutExpired(["podman", "logs"], 60),
                (b"", None),
            ]
            with self.assertRaisesRegex(under_test.ContainerError, "logs timed out"):
                list(container.logs())
            process.kill.assert_called_once()

    def test_invalid_wait_output_reports_runtime_error(self):
        container = self.runtime.run("almalinux:9", ["true"], [], {})
        self.run.return_value.stdout = "not an exit code"
        with self.assertRaisesRegex(under_test.ContainerError, "Invalid podman wait exit code"):
            container.wait()

    def test_close_removes_private_registry_credentials(self):
        self.runtime.login("username", "secret-password")
        auth_directory = self.runtime.auth_directory
        self.assertIsNotNone(auth_directory)
        with mock.patch.object(auth_directory, "cleanup", wraps=auth_directory.cleanup) as cleanup:
            self.runtime.close()
            cleanup.assert_called_once()
        self.assertIsNone(self.runtime.auth_directory)

    def test_missing_podman_reports_runtime_error(self):
        self.run.side_effect = FileNotFoundError("podman is not installed")
        with self.assertRaisesRegex(under_test.ContainerError, "podman is not installed"):
            under_test.PodmanRuntime()


@unittest.skipUnless(sys.platform == "linux", "Evergreen teardown requires Linux")
class PodmanForcedTerminationTest(unittest.TestCase):
    def test_teardown_removes_credentials_after_sigkill_even_when_podman_fails(self):
        repository = Path(under_test.__file__).resolve().parents[2]
        for cleanup_exit in (0, 125):
            with (
                self.subTest(cleanup_exit=cleanup_exit),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                state = root / "task-3"
                other_auth = root / "task-2" / "credentials" / "auth.json"
                other_auth.parent.mkdir(parents=True)
                other_auth.write_text("fixture")
                podman = root / "podman"
                podman.write_text(
                    '#!/usr/bin/env bash\nset -euo pipefail\ncase "$1" in\n'
                    'info) exit 0 ;;\nlogin) cat > "$3" ;;\n'
                    f"ps) exit {cleanup_exit} ;;\nesac\n"
                )
                podman.chmod(0o700)
                sudo = root / "sudo"
                sudo.write_text("#!/usr/bin/env bash\nexit 0\n")
                sudo.chmod(0o700)
                environment = {
                    **os.environ,
                    "PATH": f"{root}:{os.environ['PATH']}",
                    "TMPDIR": str(root),
                    "task_id": "task",
                    "execution": "3",
                    "MONGO_PACKAGE_TEST_RUNTIME": "podman",
                    "MONGO_PACKAGE_TEST_TASK_ID": "task-3",
                    "MONGO_PACKAGE_TEST_STATE_DIR": str(state),
                }
                child = subprocess.run(
                    [
                        sys.executable,
                        "-c",
                        "import os, signal\n"
                        "from buildscripts.prepare_podman_for_bazel import prepare_podman_for_package_tests\n"
                        "from buildscripts.package_test.package_test_container import PodmanRuntime\n"
                        "assert prepare_podman_for_package_tests() == 0\n"
                        "runtime = PodmanRuntime()\n"
                        "runtime.login('fixture', 'fixture')\n"
                        "os.kill(os.getpid(), signal.SIGKILL)\n",
                    ],
                    env=environment,
                    cwd=repository,
                    capture_output=True,
                    timeout=15,
                )
                self.assertEqual(-signal.SIGKILL, child.returncode, child.stderr)
                self.assertEqual(1, len(list((state / "credentials").glob("*/auth.json"))))
                cleanup = subprocess.run(
                    ["bash", str(repository / "evergreen" / "package_test_cleanup.sh")],
                    env=environment,
                    capture_output=True,
                    timeout=15,
                )
                self.assertEqual(cleanup_exit, cleanup.returncode, cleanup.stderr)
                self.assertFalse((state / "credentials").exists())
                self.assertTrue(other_auth.exists())
                self.assertEqual(cleanup_exit != 0, (state / "active").exists())


class ContainerRuntimeSelectionTest(unittest.TestCase):
    def test_unknown_runtime_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unknown container runtime"):
            under_test.create_container_runtime("other")


class ContainerTestRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.runtime = mock.Mock()
        self.scope = under_test.ContainerTestRuntime(self.runtime)
        self.addCleanup(self.scope.close)

    def test_close_removes_running_container_once(self):
        container = self.scope.run("image", ["sleep", "1000"], [], {})
        self.scope.close()
        container.remove()
        self.runtime.run.return_value.remove.assert_called_once()
        self.runtime.close.assert_not_called()

    def test_close_prevents_late_container_creation(self):
        self.scope.close()
        with self.assertRaisesRegex(under_test.ContainerError, "canceled"):
            self.scope.run("image", ["true"], [], {})
        self.runtime.run.assert_not_called()

    def test_container_started_during_cancellation_is_removed(self):
        started = threading.Event()
        release = threading.Event()
        errors = []

        def start_container(*args, **kwargs):
            started.set()
            if not release.wait(5):
                raise AssertionError("Test did not release container creation")
            return self.runtime.run.return_value

        def run():
            try:
                self.scope.run("image", ["sleep", "1000"], [], {})
            except under_test.ContainerError as exc:
                errors.append(str(exc))

        self.runtime.run.side_effect = start_container
        worker = threading.Thread(target=run)
        worker.start()
        try:
            self.assertTrue(started.wait(5))
            self.scope.close()
        finally:
            release.set()
            worker.join(5)
        self.assertFalse(worker.is_alive())
        self.assertEqual(["Package test was canceled"], errors)
        self.runtime.run.return_value.remove.assert_called_once()

    def test_failed_cleanup_can_be_retried(self):
        container = self.scope.run("image", ["true"], [], {})
        self.runtime.run.return_value.remove.side_effect = [
            under_test.ContainerError("error"),
            None,
        ]
        with self.assertRaises(under_test.ContainerError):
            self.scope.close()
        container.remove()
        self.assertEqual(2, self.runtime.run.return_value.remove.call_count)


if __name__ == "__main__":
    unittest.main()
