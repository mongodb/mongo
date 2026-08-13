from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from buildscripts import prepare_podman_for_bazel


class PreparePodmanForBazelTest(unittest.TestCase):
    def test_native_build_does_not_use_podman(self) -> None:
        for disabled_value in ("0", "false", "no", "off"):
            with self.subTest(disabled_value=disabled_value):
                self.assertFalse(
                    prepare_podman_for_bazel.podman_is_in_use(
                        {"MONGO_LINUX_CONTAINER_ACTIONS": disabled_value},
                        system="Linux",
                        which=mock.Mock(return_value="/usr/bin/podman"),
                    )
                )

    def test_healthy_docker_is_selected_over_podman(self) -> None:
        def which(command: str) -> str | None:
            return {"docker": "/usr/bin/docker", "podman": "/usr/bin/podman"}.get(command)

        def runner(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            self.assertIn(argv, [["/usr/bin/docker", "--version"], ["/usr/bin/docker", "info"]])
            return subprocess.CompletedProcess(argv, 0, "", "")

        self.assertFalse(
            prepare_podman_for_bazel.podman_is_in_use(
                {}, system="Linux", which=which, runner=runner
            )
        )

    def test_podman_is_selected_when_docker_is_unavailable(self) -> None:
        def which(command: str) -> str | None:
            return {"docker": "/usr/bin/docker", "podman": "/usr/bin/podman"}.get(command)

        def runner(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if argv[-1] == "--version":
                return subprocess.CompletedProcess(argv, 0, "Docker version 27", "")
            return subprocess.CompletedProcess(argv, 1, "", "daemon unavailable")

        self.assertTrue(
            prepare_podman_for_bazel.podman_is_in_use(
                {}, system="Linux", which=which, runner=runner
            )
        )

    def test_prepare_uses_task_scoped_podman_storage(self) -> None:
        calls: list[tuple[list[str], dict[str, object]]] = []

        def runner(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append((argv, kwargs))
            return subprocess.CompletedProcess(argv, 0, "", "")

        def which(command: str) -> str | None:
            return "/usr/bin/podman" if command == "podman" else None

        with tempfile.TemporaryDirectory() as temp_dir:
            with mock.patch.object(
                prepare_podman_for_bazel.shutil,
                "which",
                return_value="/usr/bin/fuse-overlayfs",
            ):
                result = prepare_podman_for_bazel.prepare_podman_for_bazel(
                    {"MONGO_PODMAN_TASK_ID": "task/123"},
                    runner=runner,
                    system="Linux",
                    which=which,
                    uid=1000,
                    runtime_root=pathlib.Path(temp_dir),
                )

            self.assertEqual(result, 0)
            self.assertEqual(
                [call[0] for call in calls],
                [
                    ["sudo", "loginctl", "enable-linger", "1000"],
                    ["podman", "system", "migrate"],
                    ["podman", "system", "reset", "--force"],
                    ["podman", "info"],
                ],
            )
            runtime_dir = calls[1][1]["env"]["XDG_RUNTIME_DIR"]
            self.assertEqual(calls[2][1]["env"]["XDG_RUNTIME_DIR"], runtime_dir)
            storage_config = pathlib.Path(calls[1][1]["env"]["CONTAINERS_STORAGE_CONF"])
            self.assertEqual(
                storage_config,
                pathlib.Path(temp_dir)
                / "mongo-linux-podman-task-task_123"
                / "mongo-linux-podman-storage-1000"
                / "storage.conf",
            )
            storage_config_text = storage_config.read_text(encoding="utf-8")
            self.assertIn('graphroot = "', storage_config_text)
            self.assertIn('rootless_storage_path = "', storage_config_text)
            self.assertIn(
                'driver = "overlay"',
                storage_config_text,
            )
            self.assertIn(
                'mount_program = "/usr/bin/fuse-overlayfs"',
                storage_config_text,
            )


if __name__ == "__main__":
    unittest.main()
