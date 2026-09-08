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

    def test_task_id_alone_does_not_override_disabled_container_actions(self) -> None:
        self.assertFalse(
            prepare_podman_for_bazel.podman_is_in_use(
                {
                    "MONGO_LINUX_CONTAINER_ACTIONS": "0",
                    "MONGO_PODMAN_TASK_ID": "task-123",
                },
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
                    ["podman", "rm", "--all", "--force"],
                    ["podman", "system", "reset", "--force"],
                    ["podman", "info"],
                ],
            )
            runtime_dir = calls[1][1]["env"]["XDG_RUNTIME_DIR"]
            self.assertEqual(calls[2][1]["env"]["XDG_RUNTIME_DIR"], runtime_dir)
            containers_config = pathlib.Path(calls[1][1]["env"]["CONTAINERS_CONF"])
            self.assertEqual(containers_config, pathlib.Path(runtime_dir) / "containers.conf")
            self.assertEqual(
                containers_config.read_text(encoding="utf-8"),
                '[engine]\ncgroup_manager = "cgroupfs"\n',
            )
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

    def test_prepare_preserves_host_registry_auth_file(self) -> None:
        calls: list[tuple[list[str], dict[str, object]]] = []

        def runner(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append((argv, kwargs))
            return subprocess.CompletedProcess(argv, 0, "", "")

        def which(command: str) -> str | None:
            return "/usr/bin/podman" if command == "podman" else None

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            auth_file = root / "runtime" / "containers" / "auth.json"
            auth_file.parent.mkdir(parents=True)
            auth_file.write_text("{}\n", encoding="utf-8")
            with mock.patch.object(
                prepare_podman_for_bazel.shutil,
                "which",
                return_value="/usr/bin/fuse-overlayfs",
            ):
                result = prepare_podman_for_bazel.prepare_podman_for_bazel(
                    {
                        "MONGO_PODMAN_TASK_ID": "task/123",
                        "XDG_RUNTIME_DIR": str(root / "runtime"),
                    },
                    runner=runner,
                    system="Linux",
                    which=which,
                    uid=1000,
                    runtime_root=root / "tmp",
                )

        self.assertEqual(result, 0)
        self.assertEqual(
            calls[1][1]["env"]["REGISTRY_AUTH_FILE"],
            str(auth_file),
        )

    def test_prepare_continues_after_stale_runtime_cleanup_warning(self) -> None:
        calls: list[list[str]] = []

        def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(argv)
            if argv == ["podman", "system", "migrate"]:
                return subprocess.CompletedProcess(argv, 125, "", "no pause process")
            return subprocess.CompletedProcess(argv, 0, "", "")

        def which(command: str) -> str | None:
            return "/usr/bin/podman" if command == "podman" else None

        with tempfile.TemporaryDirectory() as temp_dir:
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
            calls[1:],
            [
                ["podman", "system", "migrate"],
                ["podman", "rm", "--all", "--force"],
                ["podman", "system", "reset", "--force"],
                ["podman", "info"],
            ],
        )

    def test_cleanup_resets_and_removes_task_scoped_storage(self) -> None:
        calls: list[tuple[list[str], dict[str, object]]] = []

        def runner(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append((argv, kwargs))
            return subprocess.CompletedProcess(argv, 0, "", "")

        def which(command: str) -> str | None:
            return "/usr/bin/podman" if command == "podman" else None

        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"
            (task_root / "leftover").mkdir(parents=True)
            result = prepare_podman_for_bazel.cleanup_podman_for_bazel(
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
                    ["podman", "system", "migrate"],
                    ["podman", "rm", "--all", "--force"],
                    ["podman", "system", "reset", "--force"],
                ],
            )
            self.assertFalse(task_root.exists())
            self.assertEqual(
                calls[0][1]["env"]["XDG_RUNTIME_DIR"],
                str(task_root / "mongo-linux-podman-runtime-1000"),
            )

    def test_cleanup_is_best_effort_when_podman_reset_fails(self) -> None:
        calls: list[list[str]] = []

        def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 125, "", "reset failed")

        def which(command: str) -> str | None:
            return "/usr/bin/podman" if command == "podman" else None

        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"
            (task_root / "leftover").mkdir(parents=True)
            result = prepare_podman_for_bazel.cleanup_podman_for_bazel(
                {"MONGO_PODMAN_TASK_ID": "task/123"},
                runner=runner,
                system="Linux",
                which=which,
                uid=1000,
                runtime_root=pathlib.Path(temp_dir),
            )

            self.assertEqual(result, 0)
            # A failed reset is retried once after any task-overlay recovery.
            self.assertEqual(len(calls), 4)
            self.assertFalse(task_root.exists())

    def test_cleanup_recovers_from_podman_panic_before_reset(self) -> None:
        calls: list[list[str]] = []

        def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(argv)
            if argv == ["podman", "system", "migrate"]:
                return subprocess.CompletedProcess(argv, -6, "", "panic: invalid pause process")
            return subprocess.CompletedProcess(argv, 0, "", "")

        def which(command: str) -> str | None:
            return "/usr/bin/podman" if command == "podman" else None

        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"
            (task_root / "leftover").mkdir(parents=True)
            result = prepare_podman_for_bazel.cleanup_podman_for_bazel(
                {"MONGO_PODMAN_TASK_ID": "task/123"},
                runner=runner,
                system="Linux",
                which=which,
                uid=1000,
                runtime_root=pathlib.Path(temp_dir),
            )

        self.assertEqual(result, 0)
        self.assertEqual(
            calls,
            [
                ["podman", "system", "migrate"],
                [
                    "podman",
                    "unshare",
                    "umount",
                    "-R",
                    "-l",
                    str(
                        task_root
                        / "mongo-linux-podman-storage-1000"
                        / "graphroot"
                        / "overlay-containers"
                    ),
                ],
                ["podman", "rm", "--all", "--force"],
                ["podman", "system", "reset", "--force"],
            ],
        )
        self.assertFalse(task_root.exists())

    def test_cleanup_unmounts_task_overlay_before_retrying_removal(self) -> None:
        calls: list[list[str]] = []

        def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, "", "")

        def which(command: str) -> str | None:
            return "/usr/bin/podman" if command == "podman" else None

        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"
            (task_root / "leftover").mkdir(parents=True)
            with mock.patch.object(
                prepare_podman_for_bazel.shutil,
                "rmtree",
                side_effect=[OSError("device or resource busy"), None],
            ):
                result = prepare_podman_for_bazel.cleanup_podman_for_bazel(
                    {"MONGO_PODMAN_TASK_ID": "task/123"},
                    runner=runner,
                    system="Linux",
                    which=which,
                    uid=1000,
                    runtime_root=pathlib.Path(temp_dir),
                )

        self.assertEqual(result, 0)
        self.assertEqual(
            calls,
            [
                ["podman", "system", "migrate"],
                ["podman", "rm", "--all", "--force"],
                ["podman", "system", "reset", "--force"],
                [
                    "podman",
                    "unshare",
                    "umount",
                    "-R",
                    "-l",
                    str(
                        task_root
                        / "mongo-linux-podman-storage-1000"
                        / "graphroot"
                        / "overlay-containers"
                    ),
                ],
            ],
        )

    def test_cleanup_without_task_id_never_touches_shared_tmp(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            marker = pathlib.Path(temp_dir) / "keep"
            marker.write_text("keep", encoding="utf-8")
            runner = mock.Mock()
            result = prepare_podman_for_bazel.cleanup_podman_for_bazel(
                {},
                runner=runner,
                system="Linux",
                which=lambda _command: "/usr/bin/podman",
                runtime_root=pathlib.Path(temp_dir),
            )

            self.assertEqual(result, 0)
            runner.assert_not_called()
            self.assertTrue(marker.exists())

    def test_cleanup_removes_stale_task_state_when_docker_is_selected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"
            (task_root / "leftover").mkdir(parents=True)

            def which(command: str) -> str | None:
                return {"docker": "/usr/bin/docker", "podman": "/usr/bin/podman"}.get(command)

            def runner(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess(argv, 0, "Docker version 27", "")

            result = prepare_podman_for_bazel.cleanup_podman_for_bazel(
                {"MONGO_PODMAN_TASK_ID": "task/123"},
                runner=runner,
                system="Linux",
                which=which,
                runtime_root=pathlib.Path(temp_dir),
            )

            self.assertEqual(result, 0)
            self.assertFalse(task_root.exists())


if __name__ == "__main__":
    unittest.main()
