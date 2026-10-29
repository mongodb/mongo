"""Unit tests for bazel.wrapper_hook.hermetic_container.volumes."""

from __future__ import annotations

import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

from bazel.wrapper_hook.hermetic_container import volumes

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


class HermeticContainerVolumeMappingTest(unittest.TestCase):
    def test_wsl_docker_maps_windows_volume_sources_to_drvfs_paths(self):
        instance = object.__new__(hermetic_container.DockerInstance)
        instance.volume_source_mode = "wsl"
        instance.wsl_drive_mount_prefix = "/mnt"

        self.assertEqual(
            instance._docker_volume_source(r"Z:\mongo\.tmp\hermetic_container"),
            "/mnt/z/mongo/.tmp/hermetic_container",
        )
        self.assertEqual(
            instance._map_volume_source(r"C:\cache\bazel:/C/cache/bazel:ro"),
            "/mnt/c/cache/bazel:/C/cache/bazel:ro",
        )

    def test_destination_paths_preserve_existing_drive_case(self):
        self.assertEqual(volumes._container_path(r"C:\repo", "Windows"), "/C/repo")
        self.assertEqual(volumes._container_path(r"c:\repo", "Windows"), "/c/repo")
        self.assertEqual(volumes._container_path(r"/repo/a\b", "Linux"), r"/repo/a\b")
        self.assertEqual(volumes._container_path(r"\repo\bazel-out", "Windows"), "/repo/bazel-out")

    def test_wsl_mapping_uses_instance_prefix_and_preserves_desktop_paths(self):
        instance = object.__new__(hermetic_container.DockerInstance)
        instance.volume_source_mode = "wsl"
        instance.wsl_drive_mount_prefix = "/custom/drives"
        self.assertEqual(instance._docker_volume_source(r"C:\repo"), "/custom/drives/c/repo")
        instance.volume_source_mode = ""
        self.assertEqual(instance._docker_volume_source(r"C:\repo"), r"C:\repo")

    def test_wsl_mapping_rejects_unsafe_prefix(self):
        instance = object.__new__(hermetic_container.DockerInstance)
        instance.volume_source_mode = "wsl"
        instance.wsl_drive_mount_prefix = "/mnt/../tmp"
        with self.assertRaises(ValueError):
            instance._docker_volume_source(r"C:\repo")

    def test_standalone_module_imports_without_repository_on_python_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = pathlib.Path(temporary)
            for name in ("hermetic_container.py", "container_paths.py"):
                shutil.copyfile(HERMETIC_CONTAINER_PATH.parent / name, destination / name)
            script = (
                "import sys; sys.path.insert(0, sys.argv[1]); import hermetic_container; "
                "instance = object.__new__(hermetic_container.DockerInstance); "
                "instance.volume_source_mode = 'wsl'; instance.wsl_drive_mount_prefix = '/mnt'; "
                "assert instance._docker_volume_source('C:/repo') == '/mnt/c/repo'"
            )
            result = subprocess.run(
                [sys.executable, "-I", "-c", script, str(destination)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
