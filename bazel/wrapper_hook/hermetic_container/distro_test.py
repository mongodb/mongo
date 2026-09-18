"""Unit tests for bazel.wrapper_hook.hermetic_container.distro."""

from __future__ import annotations

import hashlib
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest
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


class DistroSelectionTest(unittest.TestCase):
    def test_detects_ubuntu_22(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            os_release = pathlib.Path(temp_dir) / "os-release"
            os_release.write_text(
                'NAME="Ubuntu"\nVERSION_ID="22.04"\n',
                encoding="utf-8",
            )

            self.assertEqual(
                hermetic_container_integration.detect_host_distro(os_release),
                "ubuntu22",
            )

    def test_detects_ubuntu_26(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            os_release = pathlib.Path(temp_dir) / "os-release"
            os_release.write_text(
                'NAME="Ubuntu"\nVERSION_ID="26.04"\n',
                encoding="utf-8",
            )

            self.assertEqual(
                hermetic_container_integration.detect_host_distro(os_release),
                "ubuntu26",
            )

    def test_detects_fixed_amazon_linux_2023_3_release(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            os_release = root / "os-release"
            system_release = root / "system-release"
            os_release.write_text(
                'NAME="Amazon Linux"\nVERSION_ID="2023"\n',
                encoding="utf-8",
            )
            system_release.write_text(
                "Amazon Linux release 2023.3.20240312 (Amazon Linux)\n",
                encoding="utf-8",
            )

            self.assertEqual(
                hermetic_container_integration.detect_host_distro(os_release, system_release),
                "amazon_linux_2023_3",
            )

    def test_detects_rolling_amazon_linux_2023_release(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            os_release = root / "os-release"
            system_release = root / "system-release"
            os_release.write_text(
                'NAME="Amazon Linux"\nVERSION_ID="2023"\n',
                encoding="utf-8",
            )
            system_release.write_text(
                "Amazon Linux release 2023.7.20250609 (Amazon Linux)\n",
                encoding="utf-8",
            )

            self.assertEqual(
                hermetic_container_integration.detect_host_distro(os_release, system_release),
                "amazon_linux_2023",
            )

    def test_selects_detected_container_when_toolchain_exists(self):
        containers = {
            "ubuntu22": {"container-url": "docker://quay.io/mongodb/rbe@sha256:1"},
            "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:2"},
        }

        selected = hermetic_container_integration.select_distro(
            containers,
            env={},
            detected_distro="ubuntu22",
            arch="aarch64",
            toolchain_supported=lambda distro, arch: distro == "ubuntu22" and arch == "aarch64",
        )

        self.assertEqual(selected, "ubuntu22")

    def test_falls_back_to_al2023_without_match(self):
        containers = {
            "ubuntu22": {"container-url": "docker://quay.io/mongodb/rbe@sha256:1"},
            "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:2"},
        }

        selected = hermetic_container_integration.select_distro(
            containers,
            env={},
            detected_distro="ubuntu22",
            arch="s390x",
            toolchain_supported=lambda _distro, _arch: False,
        )

        self.assertEqual(selected, "amazon_linux_2023")

    def test_override_must_be_known_container(self):
        with self.assertRaisesRegex(RuntimeError, "not a known RBE container"):
            hermetic_container_integration.select_distro(
                {"amazon_linux_2023": {}},
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "not_real"},
            )


class DockerImageTest(unittest.TestCase):
    def test_parses_digest_image(self):
        image = hermetic_container_integration.parse_docker_image(
            "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
        )

        self.assertEqual(image.full_name, "quay.io/mongodb/bazel-remote-execution@sha256:abc123")
        self.assertEqual(image.repository, "quay.io/mongodb")
        self.assertEqual(image.image_name, "bazel-remote-execution@sha256:abc123")
        self.assertEqual(image.digest_or_tag, "abc123")

    def test_parses_tag_image(self):
        image = hermetic_container_integration.parse_docker_image("quay.io/mongodb/rbe:ubuntu22")

        self.assertEqual(image.repository, "quay.io/mongodb")
        self.assertEqual(image.image_name, "rbe:ubuntu22")
        self.assertEqual(image.digest_or_tag, "ubuntu22")

    def test_derived_image_tag_includes_built_image_identity(self):
        base_image = hermetic_container_integration.parse_docker_image(
            "quay.io/mongodb/rbe@sha256:abc123"
        )
        image_id = "sha256:" + "a" * 64
        with tempfile.TemporaryDirectory() as temp_dir:
            completed = [
                subprocess.CompletedProcess(args=[], returncode=1, stdout="", stderr=""),
                subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr=""),
                subprocess.CompletedProcess(args=[], returncode=0, stdout=image_id, stderr=""),
                subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr=""),
            ]
            with mock.patch.object(
                hermetic_container_integration.subprocess,
                "run",
                side_effect=completed,
            ) as run:
                image, dockerfile = (
                    hermetic_container_integration._hermetic_container_image_with_git_layer(
                        pathlib.Path(temp_dir), base_image, docker_command="docker"
                    )
                )
                self.assertEqual(
                    image.full_name,
                    "mongo-hermetic_container-local/bazel-remote-execution-git:git-" + "a" * 64,
                )
                self.assertTrue(pathlib.Path(dockerfile).is_file())
                layer_tag = hashlib.sha256(pathlib.Path(dockerfile).read_bytes()).hexdigest()[:16]
                self.assertEqual(
                    run.call_args_list[1].args[0][:4],
                    [
                        "docker",
                        "build",
                        "-t",
                        "mongo-hermetic_container-local/bazel-remote-execution-git:" + layer_tag,
                    ],
                )

    def test_derived_image_build_retries_transient_network_failures(self):
        base_image = hermetic_container_integration.parse_docker_image(
            "quay.io/mongodb/rbe@sha256:abc123"
        )
        image_id = "sha256:" + "a" * 64
        with tempfile.TemporaryDirectory() as temp_dir:
            completed = [
                subprocess.CompletedProcess(args=[], returncode=1, stdout="", stderr=""),
                subprocess.CompletedProcess(args=[], returncode=1, stdout="", stderr=""),
                subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr=""),
                subprocess.CompletedProcess(args=[], returncode=0, stdout=image_id, stderr=""),
                subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr=""),
            ]
            with (
                mock.patch.object(
                    hermetic_container_integration.subprocess, "run", side_effect=completed
                ) as run,
                mock.patch.object(hermetic_container_integration.time, "sleep") as sleep,
            ):
                hermetic_container_integration._hermetic_container_image_with_git_layer(
                    pathlib.Path(temp_dir), base_image, docker_command="docker"
                )

        self.assertEqual(
            [call.args[0][1] for call in run.call_args_list[1:3]],
            ["build", "build"],
        )
        sleep.assert_called_once_with(
            hermetic_container_integration.CONTAINER_NETWORK_RETRY_DELAY_SECONDS
        )


if __name__ == "__main__":
    unittest.main()
