"""Unit tests for bazel.wrapper_hook.hermetic_container.cleanup."""

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


class HermeticContainerCleanTest(unittest.TestCase):
    def test_clean_hermetic_container_outputs_preserves_container_mount_sources(self):
        class FakeDockerInstance:
            docker_command = "docker"
            instance_name = "mongo_hermetic_container_test"
            workspace_hex_digest = "container-hash"
            bazel_output_base_digest = "output-base-hash"

            def __init__(self):
                self.commands = []
                self.sent_commands = []

            def is_running(self):
                return True

            def send_command(self, args):
                self.sent_commands.append(args)
                return 0

            def _with_docker_machine(self, command):
                return command

            def _run_silent_command(self, command, ignore_output=False):
                self.commands.append((command, ignore_output))
                return 0

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_root = root / "out"
            output_base = output_root / "output-base-hash"
            preserved_dirs = [
                output_base / "action_cache",
                output_base / "execroot",
                output_base / "external",
                output_base / hermetic_container_integration.REPO_ROOT.name,
            ]
            for preserved_dir in preserved_dirs:
                stale_file = preserved_dir / "stale"
                stale_file.parent.mkdir(parents=True)
                stale_file.write_text("stale", encoding="utf-8")
            stale_output = output_base / "bazel-out" / "target"
            stale_output.parent.mkdir(parents=True)
            stale_output.write_text("stale", encoding="utf-8")
            stale_install = output_root / "install" / "target"
            stale_install.parent.mkdir(parents=True)
            stale_install.write_text("stale", encoding="utf-8")
            run_file = root / "container.run"
            run_file.write_text("running", encoding="utf-8")
            image = hermetic_container_integration.parse_docker_image(
                "quay.io/mongodb/rbe@sha256:abc123"
            )
            config = hermetic_container_integration.HermeticContainerConfig(
                distro="amazon_linux_2023",
                docker_image=image,
                instance_name="mongo_hermetic_container_test",
                bazel_real="/tmp/bazel-darwin",
                bazel_command="/tmp/bazel-linux",
                bazel_user_output_root=str(output_root),
                hermetic_container_run_file=str(run_file),
                user="1:1",
                volumes=[],
                env_vars=[],
                platform="",
                privileged=False,
            )
            instance = FakeDockerInstance()

            rc = hermetic_container_integration._clean_hermetic_container_outputs(instance, config)

            self.assertEqual(rc, 0)
            self.assertEqual(instance.sent_commands, [["shutdown"]])
            self.assertEqual(instance.commands, [])
            self.assertTrue(output_root.is_dir())
            self.assertTrue(run_file.exists())
            for preserved_dir in preserved_dirs:
                self.assertTrue(preserved_dir.is_dir())
                self.assertEqual(list(preserved_dir.iterdir()), [])
            self.assertFalse(stale_output.exists())
            self.assertTrue(stale_install.exists())

    def test_expunge_hermetic_container_outputs_stops_container_and_removes_output_root(self):
        class FakeDockerInstance:
            docker_command = "docker"
            instance_name = "mongo_hermetic_container_test"

            def __init__(self):
                self.commands = []

            def _with_docker_machine(self, command):
                return command

            def _run_silent_command(self, command, ignore_output=False):
                self.commands.append((command, ignore_output))
                return 0

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            output_root = root / "out"
            output_file = output_root / "workspace" / "bazel-bin" / "target"
            output_file.parent.mkdir(parents=True)
            output_file.write_text("stale", encoding="utf-8")
            run_file = root / "container.run"
            run_file.write_text("running", encoding="utf-8")
            image = hermetic_container_integration.parse_docker_image(
                "quay.io/mongodb/rbe@sha256:abc123"
            )
            config = hermetic_container_integration.HermeticContainerConfig(
                distro="amazon_linux_2023",
                docker_image=image,
                instance_name="mongo_hermetic_container_test",
                bazel_real="/tmp/bazel-darwin",
                bazel_command="/tmp/bazel-linux",
                bazel_user_output_root=str(output_root),
                hermetic_container_run_file=str(run_file),
                user="1:1",
                volumes=[],
                env_vars=[],
                platform="",
                privileged=False,
            )
            instance = FakeDockerInstance()

            rc = hermetic_container_integration._expunge_hermetic_container_outputs(
                instance, config
            )

            self.assertEqual(rc, 0)
            self.assertEqual(
                instance.commands,
                [
                    ("docker stop mongo_hermetic_container_test", True),
                    ("docker rm mongo_hermetic_container_test", True),
                ],
            )
            self.assertTrue(output_root.is_dir())
            self.assertEqual(list(output_root.iterdir()), [])
            self.assertFalse(run_file.exists())


if __name__ == "__main__":
    unittest.main()
