"""Regression tests for container_mounts."""

from __future__ import annotations

import pathlib
import tempfile
import unittest
from unittest import mock

from bazel.toolchains.cc import container_action as shared
from bazel.toolchains.cc import container_mounts as helpers
from bazel.toolchains.cc.container_engine import DOCKER_HOST_MODE_ENV, WSL_LOCAL_DOCKER_HOST_MODE


class HelpersTest(unittest.TestCase):
    def test_windows_paths_are_mapped_into_linux_action_container(self):
        self.assertEqual(
            helpers.container_path(r"Z:\bazel-output\sg3oerli"),
            "/z/bazel-output/sg3oerli",
        )

        with mock.patch.dict(
            helpers.os.environ,
            {
                DOCKER_HOST_MODE_ENV: "wsl",
                helpers.WSL_DRIVE_MOUNT_PREFIX_ENV: "/mnt",
            },
            clear=True,
        ):
            self.assertEqual(
                helpers.wsl_volume_source(r"Z:\bazel-output\sg3oerli"),
                "/mnt/z/bazel-output/sg3oerli",
            )

        with (
            mock.patch.dict(
                helpers.os.environ,
                {
                    DOCKER_HOST_MODE_ENV: "wsl-local",
                    helpers.WSL_DRIVE_MOUNT_PREFIX_ENV: "/mnt",
                },
                clear=True,
            ),
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.object(
                pathlib.Path, "resolve", return_value=pathlib.Path(r"Z:\bazel-output\sg3oerli")
            ),
        ):
            self.assertEqual(
                helpers._docker_volume_source(pathlib.Path(r"Z:\bazel-output\sg3oerli")),
                "/mnt/z/bazel-output/sg3oerli",
            )

    def test_windows_wsl_mounts_preserve_absolute_symlink_targets(self):
        with (
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(
                helpers.os.environ,
                {
                    DOCKER_HOST_MODE_ENV: "wsl-local",
                    helpers.WSL_DRIVE_MOUNT_PREFIX_ENV: "/mnt",
                },
                clear=True,
            ),
        ):
            mounts = helpers.wsl_absolute_path_mounts(
                [
                    pathlib.PureWindowsPath("C:/Users/Administrator/mongo"),
                    pathlib.PureWindowsPath("Z:/bazel-output/sg3oerli"),
                    pathlib.PureWindowsPath("Z:/bazel-output/sg3oerli/execroot/_main"),
                ]
            )

        self.assertEqual(
            mounts,
            [
                "-v",
                "/mnt/z/bazel-output/sg3oerli:/mnt/z/bazel-output/sg3oerli",
                "-v",
                "/mnt/c/Users/Administrator/mongo:/mnt/c/Users/Administrator/mongo",
            ],
        )

    def test_windows_drive_letters(self):
        self.assertEqual(
            helpers._windows_drive_letters(
                [
                    pathlib.Path(r"Z:\b\b"),
                    r"C:\data\mci\src\.tmp\hermetic_container\windows-sysroots\x",
                    "z:/data/mci/src",
                    None,
                    "/not/windows",
                ]
            ),
            ["c", "z"],
        )

    def test_should_mount_drives_only_for_tcp_wsl_engines_on_windows(self):
        with (
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(
                helpers.os.environ,
                {
                    DOCKER_HOST_MODE_ENV: "wsl",
                    "DOCKER_HOST": "tcp://127.0.0.1:2375",
                },
                clear=True,
            ),
        ):
            self.assertTrue(helpers._should_mount_drives_in_docker_vm())
        with (
            mock.patch.object(helpers.platform, "system", return_value="Windows"),
            mock.patch.dict(
                helpers.os.environ,
                {DOCKER_HOST_MODE_ENV: "wsl"},
                clear=True,
            ),
        ):
            self.assertFalse(helpers._should_mount_drives_in_docker_vm())
        with (
            mock.patch.object(helpers.platform, "system", return_value="Linux"),
            mock.patch.dict(
                helpers.os.environ,
                {
                    DOCKER_HOST_MODE_ENV: "wsl",
                    "DOCKER_HOST": "tcp://127.0.0.1:2375",
                },
                clear=True,
            ),
        ):
            self.assertFalse(helpers._should_mount_drives_in_docker_vm())

    def test_container_preflight_checks_response_file_inputs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cwd = pathlib.Path(temp_dir)
            response_file = cwd / "bazel-out" / "params"
            library = cwd / "bazel-out" / "x64_windows-fastbuild" / "wiredtiger.lib"
            response_file.parent.mkdir(parents=True)
            library.parent.mkdir(parents=True)
            response_file.write_text("bazel-out/x64_windows-fastbuild/wiredtiger.lib\n")
            library.touch()

            with (
                mock.patch.object(helpers.platform, "system", return_value="Windows"),
                mock.patch.object(
                    shared,
                    "missing_container_path_indices",
                    return_value=set(),
                ) as run,
            ):
                self.assertTrue(
                    helpers.verify_container_action_inputs(
                        ["docker"],
                        "mongo-cross-action",
                        cwd,
                        # On Windows, pathlib.Path("/mongo-execroot") becomes
                        # a WindowsPath that stringifies as "\\mongo-execroot".
                        pathlib.Path(r"\mongo-execroot"),
                        ["@bazel-out/params"],
                    )
                )

        checked_paths = run.call_args.args[2]
        self.assertIn("/mongo-execroot/bazel-out/params", checked_paths)
        self.assertIn(
            "/mongo-execroot/bazel-out/x64_windows-fastbuild/wiredtiger.lib", checked_paths
        )

    def test_container_preflight_rejects_invisible_host_input(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cwd = pathlib.Path(temp_dir)
            response_file = cwd / "params"
            response_file.write_text("bazel-out/wiredtiger.lib\n")
            (cwd / "bazel-out").mkdir()
            (cwd / "bazel-out" / "wiredtiger.lib").touch()

            with mock.patch.object(
                shared,
                "missing_container_path_indices",
                return_value={2},
            ):
                self.assertFalse(
                    helpers.verify_container_action_inputs(
                        ["docker"],
                        "mongo-cross-action",
                        cwd,
                        pathlib.Path(r"\mongo-execroot"),
                        ["@params"],
                    )
                )

    def test_windows_container_mounts_resolved_execroot_aliases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            execroot = root / "output-base" / "execroot" / "_main"
            bazel_out = root / "output-base" / "actual-bazel-out"
            external = root / "output-base" / "actual-external"
            execroot.mkdir(parents=True)
            bazel_out.mkdir()
            external.mkdir()
            (execroot / "bazel-out").symlink_to(bazel_out, target_is_directory=True)
            (execroot / "external").symlink_to(external, target_is_directory=True)

            with mock.patch.object(helpers.platform, "system", return_value="Windows"):
                mounts = helpers.execroot_alias_mounts(execroot, pathlib.Path("/mongo-execroot"))

        self.assertEqual(
            mounts,
            [
                "-v",
                f"{bazel_out}:/mongo-execroot/bazel-out",
                "-v",
                f"{external}:/mongo-execroot/external",
            ],
        )

    def test_windows_container_mounts_direct_execroot_aliases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            execroot = pathlib.Path(temp_dir).resolve() / "output-base" / "execroot" / "_main"
            execroot.mkdir(parents=True)
            (execroot / "bazel-out").mkdir()
            (execroot / "external").mkdir()

            with (
                mock.patch.object(helpers.platform, "system", return_value="Windows"),
                mock.patch.dict(
                    helpers.os.environ,
                    {DOCKER_HOST_MODE_ENV: WSL_LOCAL_DOCKER_HOST_MODE},
                    clear=True,
                ),
            ):
                mounts = helpers.execroot_alias_mounts(execroot, pathlib.Path("/mongo-execroot"))

        self.assertEqual(
            mounts,
            [
                "-v",
                f"{execroot / 'bazel-out'}:/mongo-execroot/bazel-out",
                "-v",
                f"{execroot / 'external'}:/mongo-execroot/external",
            ],
        )

    def test_windows_wsl_container_mounts_raw_execroot_aliases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir).resolve()
            execroot = root / "output-base" / "execroot" / "_main"
            bazel_out = root / "output-base" / "actual-bazel-out"
            external = root / "output-base" / "actual-external"
            execroot.mkdir(parents=True)
            bazel_out.mkdir()
            external.mkdir()
            (execroot / "bazel-out").symlink_to(bazel_out, target_is_directory=True)
            (execroot / "external").symlink_to(external, target_is_directory=True)

            with (
                mock.patch.object(helpers.platform, "system", return_value="Windows"),
                mock.patch.dict(
                    helpers.os.environ,
                    {DOCKER_HOST_MODE_ENV: WSL_LOCAL_DOCKER_HOST_MODE},
                    clear=True,
                ),
            ):
                mounts = helpers.execroot_alias_mounts(execroot, pathlib.Path("/mongo-execroot"))

        self.assertEqual(
            mounts,
            [
                "-v",
                f"{execroot / 'bazel-out'}:/mongo-execroot/bazel-out",
                "-v",
                f"{execroot / 'external'}:/mongo-execroot/external",
            ],
        )

    def test_mount_prefix_rejects_traversal_and_shell_metacharacters(self):
        for prefix in ["/mnt/../tmp", "/mnt/./drives", "/mnt;touch /tmp/oops", "relative", "/"]:
            with (
                self.subTest(prefix=prefix),
                mock.patch.dict(
                    helpers.os.environ, {helpers.WSL_DRIVE_MOUNT_PREFIX_ENV: prefix}, clear=True
                ),
            ):
                with self.assertRaises(ValueError):
                    helpers.wsl_volume_source("C:/repo")

    def test_unc_paths_are_rejected_before_wsl_mounting(self):
        with self.assertRaises(ValueError):
            helpers.wsl_volume_source(r"\\server\share\repo")

    def test_input_wait_stops_at_wall_clock_deadline(self):
        with (
            mock.patch.object(
                helpers, "verify_container_action_inputs", return_value=False
            ) as verify,
            mock.patch.object(helpers.time, "monotonic", side_effect=[0, 0, 1, 1, 2]),
            mock.patch.object(helpers.time, "sleep") as sleep,
        ):
            self.assertFalse(
                helpers.wait_for_container_action_inputs(
                    ["docker"],
                    "name",
                    pathlib.Path("/repo"),
                    pathlib.Path("/container"),
                    [],
                    timeout=2,
                )
            )
        self.assertEqual(verify.call_count, 2)
        self.assertTrue(all(call.kwargs["deadline"] == 2 for call in verify.call_args_list))
        sleep.assert_called_once_with(1.0)

    def test_preflight_maps_manifest_indices_on_windows_and_macos(self):
        for system in ("Windows", "Darwin"):
            with (
                self.subTest(system=system),
                mock.patch.object(helpers.platform, "system", return_value=system),
                mock.patch.object(
                    shared, "missing_container_path_indices", return_value={1}
                ) as run,
            ):
                missing = helpers._missing_container_paths(
                    ["docker"],
                    "name",
                    [
                        (pathlib.Path("/host/ok"), "/container/ok"),
                        (pathlib.Path("/host/missing"), "/container/missing"),
                    ],
                    deadline=42,
                )
            self.assertEqual(missing, [(pathlib.Path("/host/missing"), "/container/missing")])
            run.assert_called_once_with(
                ["docker"], "name", ["/container/ok", "/container/missing"], deadline=42
            )

    def test_input_wait_stops_when_verification_consumes_deadline(self):
        with (
            mock.patch.object(
                helpers, "verify_container_action_inputs", return_value=False
            ) as verify,
            mock.patch.object(helpers.time, "monotonic", side_effect=[0, 0, 2]),
            mock.patch.object(helpers.time, "sleep") as sleep,
        ):
            self.assertFalse(
                helpers.wait_for_container_action_inputs(
                    ["docker"],
                    "name",
                    pathlib.Path("/repo"),
                    pathlib.Path("/container"),
                    [],
                    timeout=2,
                )
            )
        verify.assert_called_once()
        sleep.assert_not_called()

    def test_preflight_transport_failure_rejects_all_inputs(self):
        paths = [(pathlib.Path("/host/input"), "/container/input")]
        with mock.patch.object(shared, "missing_container_path_indices", return_value=None):
            self.assertEqual(
                helpers._missing_container_paths(["docker"], "name", paths, deadline=42), paths
            )


if __name__ == "__main__":
    unittest.main()
