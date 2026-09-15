"""Unit tests for bazel.wrapper_hook.hermetic_container.volumes."""

from __future__ import annotations

import importlib.util
import ntpath
import pathlib
import unittest
from unittest import mock

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

        with mock.patch.object(
            hermetic_container.os.path, "splitdrive", side_effect=ntpath.splitdrive
        ):
            self.assertEqual(
                instance._docker_volume_source(r"Z:\mongo\.tmp\hermetic_container"),
                "/mnt/z/mongo/.tmp/hermetic_container",
            )
            self.assertEqual(
                instance._map_volume_source(r"C:\cache\bazel:/C/cache/bazel:ro"),
                "/mnt/c/cache/bazel:/C/cache/bazel:ro",
            )


if __name__ == "__main__":
    unittest.main()
