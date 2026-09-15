"""Unit tests for bazel.wrapper_hook.hermetic_container.docker."""

from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import unittest
from contextlib import redirect_stderr
from io import StringIO
from unittest import mock

from bazel.wrapper_hook import hermetic_container_integration

HERMETIC_CONTAINER_PATH = (
    pathlib.Path(__file__).parents[2] / "hermetic_container" / "hermetic_container.py"
)
HERMETIC_CONTAINER_SPEC = importlib.util.spec_from_file_location(
    "hermetic_container_under_test", HERMETIC_CONTAINER_PATH
)
assert HERMETIC_CONTAINER_SPEC is not None
hermetic_container = importlib.util.module_from_spec(HERMETIC_CONTAINER_SPEC)
assert HERMETIC_CONTAINER_SPEC.loader is not None
HERMETIC_CONTAINER_SPEC.loader.exec_module(hermetic_container)


class DockerDaemonCheckTest(unittest.TestCase):
    def test_docker_daemon_status_reports_unreachable_daemon(self):
        with mock.patch.object(
            hermetic_container_integration.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                args=["docker", "info"],
                returncode=1,
                stdout="",
                stderr="Cannot connect to the Docker daemon",
            ),
        ):
            ok, detail = hermetic_container_integration._docker_daemon_status("docker")

        self.assertFalse(ok)
        self.assertIn("Cannot connect to the Docker daemon", detail)

    def test_docker_daemon_status_reports_timeout(self):
        with mock.patch.object(
            hermetic_container_integration.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(["docker", "info"], timeout=10),
        ):
            ok, detail = hermetic_container_integration._docker_daemon_status("docker")

        self.assertFalse(ok)
        self.assertIn("timed out after", detail)

    def test_docker_daemon_status_retries_with_supported_api_version(self):
        calls = []

        def fake_run(*args, **kwargs):
            calls.append(kwargs.get("env"))
            if len(calls) == 1:
                return subprocess.CompletedProcess(
                    args=args[0],
                    returncode=1,
                    stdout="",
                    stderr=(
                        "Error response from daemon: client version 1.54 is too new. "
                        "Maximum supported API version is 1.52"
                    ),
                )
            return subprocess.CompletedProcess(
                args=args[0], returncode=0, stdout="29.1.3", stderr=""
            )

        with (
            mock.patch.dict(hermetic_container_integration.os.environ, {}, clear=True),
            mock.patch.object(
                hermetic_container_integration.subprocess, "run", side_effect=fake_run
            ),
        ):
            ok, detail = hermetic_container_integration._docker_daemon_status("docker")

            self.assertTrue(ok)
            self.assertEqual(detail, "")
            self.assertEqual(
                hermetic_container_integration.os.environ["DOCKER_API_VERSION"], "1.52"
            )

        self.assertIsNone(calls[0])
        self.assertEqual(calls[1]["DOCKER_API_VERSION"], "1.52")

    def test_run_hermetic_container_reports_docker_daemon_error_without_loading_hermetic_container(
        self,
    ):
        image = hermetic_container_integration.parse_docker_image(
            "quay.io/mongodb/rbe@sha256:abc123"
        )
        config = hermetic_container_integration.HermeticContainerConfig(
            distro="amazon_linux_2023",
            docker_image=image,
            instance_name="test",
            bazel_real="/tmp/bazel-darwin",
            bazel_command="/tmp/bazel-linux",
            bazel_user_output_root="/tmp/output",
            hermetic_container_run_file="/tmp/run",
            user="1:1",
            volumes=[],
            env_vars=[],
            platform="",
            privileged=False,
        )

        stderr = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Darwin"
            ),
            mock.patch.object(
                hermetic_container_integration.shutil, "which", return_value="/usr/bin/docker"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "build_hermetic_container_config",
                return_value=config,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_docker_daemon_status",
                return_value=(False, "Cannot connect to the Docker daemon"),
            ),
            mock.patch.object(
                hermetic_container_integration, "_load_hermetic_container_module"
            ) as load_hermetic_container,
            redirect_stderr(stderr),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/tmp/bazel-darwin",
                ["build", "install-dist-test"],
                env={"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1"},
            )

        self.assertEqual(rc, 1)
        self.assertIn("Start Docker Desktop", stderr.getvalue())
        self.assertIn("Cannot connect to the Docker daemon", stderr.getvalue())
        load_hermetic_container.assert_not_called()


if __name__ == "__main__":
    unittest.main()
