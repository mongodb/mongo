"""Unit tests for bazel.wrapper_hook.hermetic_container.env."""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

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


class ContainerDetectionTest(unittest.TestCase):
    def test_detects_container_markers_and_cgroups(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            marker = root / ".dockerenv"
            cgroup = root / "cgroup"

            marker.touch()
            self.assertTrue(
                hermetic_container_integration._is_running_in_container(
                    env={}, container_marker_paths=(marker,), cgroup_path=root / "missing"
                )
            )

            marker.unlink()
            cgroup.write_text("0::/kubepods.slice/kubepods-burstable.slice\n", encoding="utf-8")
            self.assertTrue(
                hermetic_container_integration._is_running_in_container(
                    env={}, container_marker_paths=(marker,), cgroup_path=cgroup
                )
            )

    def test_detects_container_environment_variable(self):
        self.assertTrue(
            hermetic_container_integration._is_running_in_container(
                env={"container": "podman"},
                container_marker_paths=(),
                cgroup_path=pathlib.Path("/definitely-not-a-container-cgroup"),
            )
        )

    def test_detects_kubernetes_with_private_cgroup_namespace(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cgroup = pathlib.Path(temp_dir) / "cgroup"
            cgroup.write_text("0::/\n", encoding="utf-8")

            self.assertFalse(
                hermetic_container_integration._is_running_in_container(
                    env={}, container_marker_paths=(), cgroup_path=cgroup
                )
            )
            self.assertTrue(
                hermetic_container_integration._is_running_in_container(
                    env={"KUBERNETES_SERVICE_HOST": "10.0.0.1"},
                    container_marker_paths=(),
                    cgroup_path=cgroup,
                )
            )


if __name__ == "__main__":
    unittest.main()
