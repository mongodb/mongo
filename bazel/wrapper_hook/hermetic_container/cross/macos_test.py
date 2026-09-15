"""Unit tests for bazel.wrapper_hook.hermetic_container.cross.macos."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from unittest import mock

from bazel.wrapper_hook import hermetic_container_integration

HERMETIC_CONTAINER_PATH = (
    pathlib.Path(__file__).parents[3] / "hermetic_container" / "hermetic_container.py"
)
HERMETIC_CONTAINER_SPEC = importlib.util.spec_from_file_location(
    "hermetic_container_under_test", HERMETIC_CONTAINER_PATH
)
assert HERMETIC_CONTAINER_SPEC is not None
hermetic_container = importlib.util.module_from_spec(HERMETIC_CONTAINER_SPEC)
assert HERMETIC_CONTAINER_SPEC.loader is not None
HERMETIC_CONTAINER_SPEC.loader.exec_module(hermetic_container)

MACOS_CROSS_ACTION_WRAPPER_PATH = (
    pathlib.Path(__file__).parents[3]
    / "toolchains"
    / "cc"
    / "mongo_apple_cross"
    / "macos_cross_action_wrapper.py"
)
MACOS_CROSS_ACTION_WRAPPER_SPEC = importlib.util.spec_from_file_location(
    "macos_cross_action_wrapper_under_test", MACOS_CROSS_ACTION_WRAPPER_PATH
)
assert MACOS_CROSS_ACTION_WRAPPER_SPEC is not None
macos_cross_action_wrapper = importlib.util.module_from_spec(MACOS_CROSS_ACTION_WRAPPER_SPEC)
assert MACOS_CROSS_ACTION_WRAPPER_SPEC.loader is not None
MACOS_CROSS_ACTION_WRAPPER_SPEC.loader.exec_module(macos_cross_action_wrapper)


class MacOSCrossActionWrapperTest(unittest.TestCase):
    def test_start_container_passes_a_single_name_option(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            output_base = repo_root / "output-base"
            output_base.mkdir()
            with (
                mock.patch.object(
                    macos_cross_action_wrapper, "_container_running", return_value=False
                ),
                mock.patch.object(
                    macos_cross_action_wrapper, "_container_exists", return_value=False
                ),
                mock.patch.object(macos_cross_action_wrapper, "_check") as check,
                mock.patch.dict(macos_cross_action_wrapper.os.environ, {}, clear=True),
            ):
                macos_cross_action_wrapper._start_container(
                    ["docker"],
                    name="mongo-cross-action",
                    image="example/image",
                    dockerfile="",
                    repo_root=repo_root,
                    output_base=output_base,
                )

        command = check.call_args.args[0]
        self.assertEqual(command.count("--name"), 1)
        name_index = command.index("--name")
        self.assertEqual(command[name_index : name_index + 2], ["--name", "mongo-cross-action"])

    def test_linux_path_uses_the_declared_python_tool(self):
        with (
            mock.patch.object(macos_cross_action_wrapper.platform, "system", return_value="Linux"),
            mock.patch.object(
                macos_cross_action_wrapper,
                "_run",
                return_value=subprocess.CompletedProcess(args=[], returncode=0),
            ) as run,
        ):
            self.assertEqual(
                macos_cross_action_wrapper.main(
                    [
                        "macos_cross_action_wrapper.py",
                        "/toolchains/python/bin/python3",
                        "buildscripts/idl/idlc.py",
                    ]
                ),
                0,
            )

        run.assert_called_once_with(["/toolchains/python/bin/python3", "buildscripts/idl/idlc.py"])

    def test_loads_host_runtime_settings_from_output_base_manifest(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_base = pathlib.Path(temp_dir) / "output-base"
            output_base.mkdir()
            (output_base / macos_cross_action_wrapper.ACTION_CONFIG_FILENAME).write_text(
                json.dumps(
                    {
                        "version": 1,
                        "environment": {
                            "MONGO_MACOS_CROSS_ACTION_REPO_ROOT": "/checkout",
                            "MONGO_MACOS_CROSS_ACTION_DOCKER_COMMAND": "/usr/bin/docker",
                        },
                    }
                ),
                encoding="utf-8",
            )

            with mock.patch.dict(macos_cross_action_wrapper.os.environ, {}, clear=True):
                macos_cross_action_wrapper._load_action_config(output_base)

                self.assertEqual(
                    macos_cross_action_wrapper.os.environ["MONGO_MACOS_CROSS_ACTION_REPO_ROOT"],
                    "/checkout",
                )
                self.assertEqual(
                    macos_cross_action_wrapper.os.environ[
                        "MONGO_MACOS_CROSS_ACTION_DOCKER_COMMAND"
                    ],
                    "/usr/bin/docker",
                )

    def test_windows_lock_fallback_does_not_require_fcntl(self):
        class Msvcrt:
            LK_LOCK = 1
            LK_UNLCK = 2

            calls: list[tuple[int, int, int]] = []

            @classmethod
            def locking(cls, descriptor: int, mode: int, size: int) -> None:
                cls.calls.append((descriptor, mode, size))

        with tempfile.TemporaryDirectory() as temp_dir:
            lock_path = pathlib.Path(temp_dir) / "container.lock"
            with (
                mock.patch.object(macos_cross_action_wrapper, "fcntl", None),
                mock.patch.object(macos_cross_action_wrapper, "msvcrt", Msvcrt),
                macos_cross_action_wrapper._exclusive_file_lock(lock_path),
            ):
                self.assertTrue(lock_path.exists())

        self.assertEqual([mode for _, mode, _ in Msvcrt.calls], [Msvcrt.LK_LOCK, Msvcrt.LK_UNLCK])


class MacOSCrossHostTestTest(unittest.TestCase):
    def test_writes_host_paths_to_runtime_manifest_not_action_env(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_base = pathlib.Path(temp_dir) / "output-base"
            path = hermetic_container_integration._write_cross_host_action_config(
                [f"--output_base={output_base}", "build"],
                {
                    "HERMETIC_CONTAINER_DOCKER_COMMAND": "/usr/local/bin/docker",
                    "DOCKER_HOST": "unix:///tmp/docker.sock",
                    "LLVM_PATH": "/Users/example/llvm",
                    "MACOS_SDK_PATH": "/Users/example/sdk",
                },
                "Darwin",
            )

            config = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(path, output_base / "mongo_cross_host_action.json")
            self.assertEqual(
                config["environment"]["MONGO_MACOS_CROSS_ACTION_DOCKER_COMMAND"],
                "/usr/local/bin/docker",
            )
            self.assertEqual(config["environment"]["DOCKER_HOST"], "unix:///tmp/docker.sock")
            self.assertEqual(config["environment"]["LLVM_PATH"], "/Users/example/llvm")
            self.assertEqual(config["environment"]["MACOS_SDK_PATH"], "/Users/example/sdk")

    def test_detects_macos_cross_host_bazel_test_mode(self):
        self.assertTrue(
            hermetic_container_integration._macos_cross_host_bazel_test_requested(
                ["test", "--config=macos-cross-arm64", "--config=remote_link", "//src:mongo_test"],
                {},
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_bazel_test_requested(
                ["test", "--config=macos-cross-arm64", "--config=remote_link", "//src:mongo_test"],
                {"MONGO_MACOS_CROSS_TEST_RUNNER": "0"},
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_bazel_test_requested(
                ["test", "--config=macos-cross-arm64", "//src:mongo_test"],
                {},
                "Darwin",
            )
        )
        self.assertTrue(
            hermetic_container_integration._macos_cross_host_bazel_test_requested(
                ["test", "--config=macos-cross-arm64", "--config=remote_link", "//src:mongo_test"],
                {"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1"},
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_bazel_test_requested(
                ["test", "--config=macos-cross-arm64", "--config=remote_link", "//src:mongo_test"],
                {},
                "Linux",
            )
        )

    def test_detects_macos_cross_split_host_test_mode(self):
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_test_requested(
                ["test", "--config=macos-cross-arm64", "//src:mongo_test"],
                {},
                "Darwin",
            )
        )
        self.assertTrue(
            hermetic_container_integration._macos_cross_host_test_requested(
                ["test", "--config=macos-cross-arm64", "//src:mongo_test"],
                {"MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER": "1"},
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_test_requested(
                ["test", "--config=macos-cross-arm64", "--config=remote_link", "//src:mongo_test"],
                {},
                "Darwin",
            )
        )
        self.assertTrue(
            hermetic_container_integration._macos_cross_host_test_requested(
                ["test", "--config=macos-cross-arm64", "--config=remote_link", "//src:mongo_test"],
                {"MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER": "1"},
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_test_requested(
                ["test", "--config=macos-cross-arm64", "//src:mongo_test"],
                {
                    "MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER": "1",
                    "MONGO_MACOS_CROSS_TEST_RUNNER": "0",
                },
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_test_requested(
                ["test", "--config=macos-cross-arm64", "//src:mongo_test"],
                {},
                "Linux",
            )
        )

    def test_detects_macos_cross_host_run_mode(self):
        self.assertTrue(
            hermetic_container_integration._macos_cross_host_run_requested(
                ["run", "--config=macos-cross-arm64", "//src:mongo_test"],
                {},
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_run_requested(
                ["run", "--config=macos-cross-arm64", "//src:mongo_test"],
                {"MONGO_MACOS_CROSS_TEST_RUNNER": "0"},
                "Darwin",
            )
        )
        self.assertFalse(
            hermetic_container_integration._macos_cross_host_run_requested(
                ["run", "--config=macos-cross-arm64", "//src:mongo_test"],
                {},
                "Linux",
            )
        )

    def test_plans_macos_cross_host_bazel_test_args(self):
        args = hermetic_container_integration._macos_cross_host_bazel_test_args(
            [
                "test",
                "--config=macos-cross-arm64",
                "--config=remote_link",
                "//src/mongo:some_test",
            ],
            {},
            containers={
                "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
            },
        )

        self.assertEqual(args[0], "test")
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", args)
        self.assertIn(
            "--remote_default_exec_properties=container-image=docker://quay.io/mongodb/rbe@sha256:abc123",
            args,
        )
        self.assertIn("--remote_default_exec_properties=dockerNetwork=standard", args)
        self.assertIn("--remote_default_exec_properties=Pool=default", args)
        self.assertIn("--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=1", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=aarch64", args)
        self.assertIn("--//bazel/config:macos_cross_linux_python_arch=aarch64", args)
        self.assertIn("--//bazel/config:idl_use_linux_python=True", args)
        self.assertIn("--//bazel/config:remote_link=True", args)
        self.assertIn("--spawn_strategy=local", args)
        self.assertIn("--strategy=MongoInstallRule=local", args)
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn("--strategy=CppLink=remote", args)
        self.assertIn("--strategy=IdlcGenerator=remote", args)
        self.assertIn("--features=-thin_archive", args)
        self.assertIn("--local_resources=cpu=HOST_CPUS", args)
        self.assertIn("--local_test_jobs=HOST_CPUS", args)
        self.assertIn("--test_strategy=standalone", args)
        self.assertIn("--strategy=TestRunner=standalone", args)
        self.assertLess(
            args.index("--test_strategy=standalone"), args.index("//src/mongo:some_test")
        )

    def test_plans_macos_cross_local_container_action_args(self):
        args = hermetic_container_integration._macos_cross_local_container_action_args(
            [
                "test",
                "--config=macos-cross-arm64",
                "//src/mongo:some_test",
            ],
            {
                "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
                "DOCKER_HOST": "unix:///tmp/docker.sock",
            },
            containers={
                "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
            },
        )

        self.assertEqual(args[0], "test")
        self.assertIn("--//bazel/config:macos_cross_local_container_actions=True", args)
        self.assertIn("--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=1", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=aarch64", args)
        self.assertIn("--//bazel/config:macos_cross_linux_python_arch=aarch64", args)
        self.assertIn("--//bazel/config:idl_use_linux_python=True", args)
        self.assertIn("--strategy=MongoInstallRule=local", args)
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", args)
        self.assertIn(
            "--remote_default_exec_properties=container-image=docker://quay.io/mongodb/rbe@sha256:abc123",
            args,
        )
        self.assertIn("--remote_default_exec_properties=dockerNetwork=standard", args)
        self.assertIn("--remote_default_exec_properties=Pool=default", args)
        self.assertIn("--spawn_strategy=local", args)
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn("--strategy=IdlcGenerator=remote", args)
        self.assertIn("--local_resources=cpu=HOST_CPUS", args)
        self.assertIn("--local_test_jobs=HOST_CPUS", args)
        self.assertNotIn("--//bazel/config:remote_link=True", args)
        self.assertIn("--strategy=CppLink=local", args)
        self.assertIn("--strategy=CppArchive=local", args)
        self.assertIn("--strategy=SolibSymlink=local", args)
        self.assertIn("--strategy=ExtractDebugInfo=local", args)
        self.assertIn("--strategy=StripDebugInfo=local", args)
        self.assertIn("--strategy=CcGenerateIntermediateDwp=local", args)
        self.assertIn("--strategy=CcGenerateDwp=local", args)
        self.assertNotIn("--strategy=CppLink=remote", args)
        self.assertNotIn("--features=-thin_archive", args)
        self.assertIn("--test_strategy=standalone", args)
        self.assertIn("--strategy=TestRunner=standalone", args)
        self.assertIn(
            "--action_env=MONGO_MACOS_CROSS_ACTION_IMAGE=quay.io/mongodb/rbe@sha256:abc123",
            args,
        )
        self.assertFalse(
            any(
                argument.startswith("--action_env=")
                and any(
                    variable in argument
                    for variable in (
                        "MONGO_MACOS_CROSS_ACTION_DOCKER_COMMAND",
                        "MONGO_MACOS_CROSS_ACTION_REPO_ROOT",
                        "MONGO_MACOS_CROSS_ACTION_HOME",
                        "MONGO_MACOS_CROSS_ACTION_WRAPPER_SCRIPT",
                        "MONGO_MACOS_CROSS_ACTION_PYTHON",
                        "LLVM_PATH",
                        "MACOS_SDK_PATH",
                        "DOCKER_HOST",
                        "DOCKER_CONTEXT",
                        "DOCKER_CONFIG",
                    )
                )
                for argument in args
            )
        )

    def test_plans_macos_cross_local_container_action_args_with_remote_link(self):
        args = hermetic_container_integration._macos_cross_local_container_action_args(
            [
                "test",
                "--config=macos-cross-arm64",
                "--config=remote_link",
                "//src/mongo:some_test",
            ],
            {
                "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
            },
            containers={
                "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
            },
        )

        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", args)
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn("--strategy=IdlcGenerator=remote", args)
        self.assertIn("--//bazel/config:remote_link=True", args)
        self.assertIn("--strategy=MongoInstallRule=local", args)
        self.assertIn("--strategy=CppLink=remote", args)
        self.assertIn("--strategy=CppArchive=remote", args)
        self.assertIn("--strategy=SolibSymlink=remote", args)
        self.assertIn("--strategy=ExtractDebugInfo=remote", args)
        self.assertIn("--strategy=StripDebugInfo=remote", args)
        self.assertIn("--strategy=CcGenerateIntermediateDwp=remote", args)
        self.assertIn("--strategy=CcGenerateDwp=remote", args)
        self.assertIn("--features=-thin_archive", args)
        self.assertIn("--local_resources=cpu=HOST_CPUS", args)
        self.assertIn("--local_test_jobs=HOST_CPUS", args)
        self.assertNotIn("--strategy=CppLink=local", args)

    def test_plans_macos_cross_local_resource_overrides(self):
        args = hermetic_container_integration._macos_cross_local_container_action_args(
            [
                "test",
                "--config=macos-cross-arm64",
                "//src/mongo:some_test",
            ],
            {
                "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
                "MONGO_MACOS_CROSS_LOCAL_CPU_RESOURCES": "HOST_CPUS*.5",
                "MONGO_MACOS_CROSS_LOCAL_TEST_JOBS": "4",
            },
            containers={
                "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
            },
        )

        self.assertIn("--local_resources=cpu=HOST_CPUS*.5", args)
        self.assertIn("--local_test_jobs=4", args)
        self.assertNotIn("--local_resources=cpu=HOST_CPUS", args)
        self.assertNotIn("--local_test_jobs=HOST_CPUS", args)

    def test_plans_macos_cross_local_container_action_args_when_remote_execution_disabled(self):
        args = hermetic_container_integration._macos_cross_local_container_action_args(
            [
                "test",
                "--config=macos-cross-arm64",
                "--config=local",
                "//src/mongo:some_test",
            ],
            {
                "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
                "DOCKER_HOST": "unix:///tmp/docker.sock",
            },
            containers={
                "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
            },
        )

        self.assertIn("--//bazel/config:macos_cross_local_container_actions=True", args)
        self.assertNotIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=aarch64", args)
        self.assertIn("--//bazel/config:macos_cross_linux_python_arch=aarch64", args)
        self.assertIn("--strategy=CppCompile=local", args)
        self.assertIn("--strategy=MongoInstallRule=local", args)
        self.assertIn("--strategy=CppLink=local", args)
        self.assertIn("--strategy=IdlcGenerator=local", args)
        self.assertIn("--local_resources=cpu=HOST_CPUS", args)
        self.assertIn("--local_test_jobs=HOST_CPUS", args)
        self.assertIn("--test_strategy=standalone", args)
        self.assertIn("--strategy=TestRunner=standalone", args)

    def test_macos_cross_host_bazel_test_args_respect_rbe_overrides(self):
        args = hermetic_container_integration._macos_cross_host_bazel_test_args(
            ["test", "--config=macos-cross-arm64", "//src/mongo:some_test"],
            {
                "MONGO_MACOS_CROSS_RBE_CONTAINER_IMAGE": "docker://example.com/custom:latest",
                "MONGO_MACOS_CROSS_RBE_POOL": "custom-pool",
            },
        )

        self.assertIn(
            "--remote_default_exec_properties=container-image=docker://example.com/custom:latest",
            args,
        )
        self.assertIn("--remote_default_exec_properties=Pool=custom-pool", args)
        self.assertIn("--strategy=MongoInstallRule=local", args)

    def test_run_hermetic_container_runs_macos_cross_test_with_host_bazel_without_docker(self):
        containers = {
            "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
        }

        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Darwin"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="arm64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "load_remote_execution_containers",
                return_value=containers,
            ),
            mock.patch.object(
                hermetic_container_integration, "_docker_daemon_status"
            ) as docker_status,
            mock.patch.object(
                hermetic_container_integration, "_run_direct", return_value=0
            ) as run_direct,
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/tmp/bazel-darwin",
                ["test", "--config=remote_link", "//src/mongo:some_test"],
                env={"MONGO_MACOS_CROSS_DEFAULT_CONFIG": "1"},
            )

        self.assertEqual(rc, 0)
        docker_status.assert_not_called()
        run_direct.assert_called_once()
        self.assertEqual(run_direct.call_args.args[0], "/tmp/bazel-darwin")
        host_args = run_direct.call_args.args[1]
        self.assertEqual(host_args[0], "test")
        self.assertIn("--config=macos-cross-arm64", host_args)
        self.assertIn("--config=remote_link", host_args)
        self.assertIn("//src/mongo:some_test", host_args)
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", host_args)
        self.assertIn(
            "--remote_default_exec_properties=container-image=docker://quay.io/mongodb/rbe@sha256:abc123",
            host_args,
        )
        self.assertIn("--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=1", host_args)
        self.assertIn("--//bazel/config:idl_use_linux_python=True", host_args)
        self.assertIn("--//bazel/config:remote_link=True", host_args)
        self.assertIn("--spawn_strategy=local", host_args)
        self.assertIn("--strategy=CppCompile=remote", host_args)
        self.assertIn("--strategy=CppLink=remote", host_args)
        self.assertIn("--strategy=IdlcGenerator=remote", host_args)
        self.assertIn("--features=-thin_archive", host_args)
        self.assertIn("--local_resources=cpu=HOST_CPUS", host_args)
        self.assertIn("--local_test_jobs=HOST_CPUS", host_args)
        self.assertIn("--test_strategy=standalone", host_args)
        self.assertIn("--strategy=TestRunner=standalone", host_args)

    def test_run_hermetic_container_uses_local_container_actions_for_macos_cross_test(self):
        containers = {
            "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
        }

        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Darwin"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="arm64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "load_remote_execution_containers",
                return_value=containers,
            ),
            mock.patch.object(
                hermetic_container_integration, "_docker_daemon_status", return_value=(True, "")
            ) as docker_status,
            mock.patch.object(
                hermetic_container_integration, "_run_direct", return_value=0
            ) as run_direct,
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/tmp/bazel-darwin",
                ["test", "//src/mongo:some_test"],
                env={
                    "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
                    "MONGO_MACOS_CROSS_DEFAULT_CONFIG": "1",
                },
            )

        self.assertEqual(rc, 0)
        docker_status.assert_called_once_with("docker")
        run_direct.assert_called_once()
        self.assertEqual(run_direct.call_args.args[0], "/tmp/bazel-darwin")
        host_args = run_direct.call_args.args[1]
        self.assertEqual(host_args[0], "test")
        self.assertIn("--config=macos-cross-arm64", host_args)
        self.assertIn("//src/mongo:some_test", host_args)
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", host_args)
        self.assertIn(
            "--remote_default_exec_properties=container-image=docker://quay.io/mongodb/rbe@sha256:abc123",
            host_args,
        )
        self.assertIn("--//bazel/config:macos_cross_local_container_actions=True", host_args)
        self.assertIn("--//bazel/config:idl_use_linux_python=True", host_args)
        self.assertNotIn("--//bazel/config:remote_link=True", host_args)
        self.assertIn("--spawn_strategy=local", host_args)
        self.assertIn("--strategy=CppCompile=remote", host_args)
        self.assertIn("--strategy=CppLink=local", host_args)
        self.assertIn("--strategy=CppArchive=local", host_args)
        self.assertIn("--strategy=IdlcGenerator=remote", host_args)
        self.assertNotIn("--strategy=CppLink=remote", host_args)
        self.assertNotIn("--features=-thin_archive", host_args)
        self.assertIn("--local_resources=cpu=HOST_CPUS", host_args)
        self.assertIn("--local_test_jobs=HOST_CPUS", host_args)
        self.assertIn("--test_strategy=standalone", host_args)
        self.assertIn("--strategy=TestRunner=standalone", host_args)
        self.assertIn(
            "--action_env=MONGO_MACOS_CROSS_ACTION_IMAGE=quay.io/mongodb/rbe@sha256:abc123",
            host_args,
        )

    def test_run_hermetic_container_checks_docker_for_local_only_macos_cross_test(self):
        containers = {
            "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
        }

        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Darwin"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="arm64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "load_remote_execution_containers",
                return_value=containers,
            ),
            mock.patch.object(
                hermetic_container_integration, "_docker_daemon_status", return_value=(True, "")
            ) as docker_status,
            mock.patch.object(
                hermetic_container_integration, "_run_direct", return_value=0
            ) as run_direct,
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/tmp/bazel-darwin",
                ["test", "--config=local", "//src/mongo:some_test"],
                env={
                    "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
                    "MONGO_MACOS_CROSS_DEFAULT_CONFIG": "1",
                },
            )

        self.assertEqual(rc, 0)
        docker_status.assert_called_once_with("docker")
        host_args = run_direct.call_args.args[1]
        self.assertIn("--config=local", host_args)
        self.assertIn("--strategy=CppCompile=local", host_args)
        self.assertIn("--strategy=CppLink=local", host_args)

    def test_run_hermetic_container_uses_split_host_test_runner_for_local_link_macos_cross_test(
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

        stdout = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Darwin"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="arm64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "build_hermetic_container_config",
                return_value=config,
            ),
            redirect_stdout(stdout),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/tmp/bazel-darwin",
                ["test", "//src/mongo:some_test"],
                env={
                    "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
                    "MONGO_MACOS_CROSS_DEFAULT_CONFIG": "1",
                    "MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER": "1",
                },
            )

        self.assertEqual(rc, 0)
        dry_run = json.loads(stdout.getvalue().splitlines()[-1])
        self.assertIn("hermetic_container_config", dry_run)
        self.assertIn("macos_cross_host_test", dry_run)
        self.assertNotIn("macos_cross_host_bazel_test", dry_run)
        self.assertEqual(
            dry_run["macos_cross_host_test"]["build_args"],
            [
                "build",
                "--build_tests_only",
                "--symlink_prefix=bazel-",
                "--config=macos-cross-arm64",
                "//src/mongo:some_test",
            ],
        )

    def test_plans_hermetic_container_build_and_host_test_args(self):
        plan = hermetic_container_integration._macos_cross_host_test_plan(
            [
                "test",
                "--config=macos-cross-arm64",
                "--test_arg=--fileNameFilter",
                "--test_arg=some_test",
                "--test_env=FROM_ARG=value",
                "--test_env",
                "FROM_HOST",
                "--test_filter=Suite.Test",
                "--test_tag_filters=mongo_unittest,-requires_large_host",
                "--build_event_json_file=build_events.json",
                "--runs_per_test=2",
                "//src/mongo:some_test",
            ],
            {"FROM_HOST": "host-value"},
        )

        self.assertEqual(
            plan.build_args,
            [
                "build",
                "--build_tests_only",
                "--config=macos-cross-arm64",
                "--test_tag_filters=mongo_unittest,-requires_large_host",
                "--build_event_json_file=build_events.json",
                "//src/mongo:some_test",
            ],
        )
        self.assertEqual(
            plan.test_args,
            ["--fileNameFilter", "some_test"],
        )
        self.assertEqual(plan.startup_args, [])
        self.assertEqual(
            plan.host_test_options,
            [
                "--test_arg=--fileNameFilter",
                "--test_arg=some_test",
                "--test_env=FROM_ARG=value",
                "--test_env=FROM_HOST",
                "--test_filter=Suite.Test",
                "--runs_per_test=2",
            ],
        )
        self.assertEqual(
            plan.test_env,
            {
                "FROM_ARG": "value",
                "FROM_HOST": "host-value",
                "TESTBRIDGE_TEST_ONLY": "Suite.Test",
            },
        )
        self.assertEqual(plan.runs_per_test, 2)
        self.assertTrue(plan.run_host_tests)
        self.assertEqual(plan.build_event_json_file, "build_events.json")
        self.assertEqual(plan.target_patterns, ["//src/mongo:some_test"])
        self.assertEqual(plan.test_tag_filters, ["mongo_unittest", "-requires_large_host"])

    def test_plans_no_build_without_host_test_execution(self):
        plan = hermetic_container_integration._macos_cross_host_test_plan(
            [
                "test",
                "--config=macos-cross-arm64",
                "--nobuild",
                "//src/mongo:some_test",
            ],
            {},
        )

        self.assertEqual(
            plan.build_args,
            [
                "build",
                "--build_tests_only",
                "--config=macos-cross-arm64",
                "--nobuild",
                "//src/mongo:some_test",
            ],
        )
        self.assertFalse(plan.run_host_tests)
        self.assertNotIn("--nobuild", plan.host_test_options)
        self.assertEqual(plan.target_patterns, ["//src/mongo:some_test"])

    def test_rejects_unsupported_cross_host_run_options(self):
        for option in ("--run_under", "--script_path"):
            with self.subTest(option=option):
                with self.assertRaisesRegex(
                    RuntimeError, f"macOS cross host run does not support {option}"
                ):
                    hermetic_container_integration._macos_cross_host_run_plan(
                        [
                            "run",
                            "--config=macos-cross-arm64",
                            option,
                            "ignored",
                            "//src/mongo:some_test",
                        ]
                    )

                with self.assertRaisesRegex(
                    RuntimeError, f"macOS cross host run does not support {option}"
                ):
                    hermetic_container_integration._macos_cross_host_run_plan(
                        [
                            "run",
                            "--config=macos-cross-arm64",
                            f"{option}=ignored",
                            "//src/mongo:some_test",
                        ]
                    )

    def test_plans_hermetic_container_build_and_host_run_args(self):
        plan = hermetic_container_integration._macos_cross_host_run_plan(
            [
                "run",
                "--config=macos-cross-arm64",
                "//src/mongo:some_test",
                "--",
                "--fileNameFilter",
                "some_test",
            ],
        )

        self.assertEqual(
            plan.build_args,
            [
                "build",
                "--config=macos-cross-arm64",
                "//src/mongo:some_test",
            ],
        )
        self.assertEqual(plan.target, "//src/mongo:some_test")
        self.assertEqual(plan.run_args, ["--fileNameFilter", "some_test"])

    def test_cross_host_run_passes_unlisted_equals_form_option(self):
        plan = hermetic_container_integration._macos_cross_host_run_plan(
            [
                "run",
                "--remote_download_outputs=minimal",
                "//src/mongo:some_test",
            ],
        )

        self.assertEqual(
            plan.build_args,
            [
                "build",
                "--remote_download_outputs=minimal",
                "//src/mongo:some_test",
            ],
        )
        self.assertEqual(plan.target, "//src/mongo:some_test")

    def test_plans_windows_cross_host_run_args(self):
        plan = hermetic_container_integration._windows_cross_host_run_plan(
            [
                "run",
                f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}",
                "//src/mongo:some_test",
                "--",
                "--fileNameFilter",
                "some_test",
            ],
        )

        self.assertEqual(
            plan.build_args,
            [
                "build",
                f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}",
                "//src/mongo:some_test",
            ],
        )
        self.assertEqual(plan.target, "//src/mongo:some_test")
        self.assertEqual(plan.run_args, ["--fileNameFilter", "some_test"])

    def test_expands_host_test_labels_with_query(self):
        plan = hermetic_container_integration.MacOSCrossHostTestPlan(
            build_args=["build", "//src/..."],
            startup_args=[],
            host_test_options=[],
            target_patterns=["//src/..."],
            test_args=[],
            test_env={},
            test_tag_filters=[],
            build_event_json_file=None,
            runs_per_test=1,
            run_host_tests=True,
        )

        with mock.patch.object(
            hermetic_container_integration.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                args=[],
                returncode=0,
                stdout="//src/mongo:one_test\n//src/mongo:two_test\n",
                stderr="",
            ),
        ):
            labels = hermetic_container_integration._expand_host_test_labels("/tmp/bazel", plan, {})

        self.assertEqual(labels, ["//src/mongo:one_test", "//src/mongo:two_test"])

    def test_expands_host_test_labels_from_build_event_json(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            build_events = repo_root / "build_events.json"
            build_events.write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "id": {"pattern": {"pattern": ["//..."]}},
                                "children": [
                                    {
                                        "targetConfigured": {
                                            "label": "//buildscripts/smoke_tests:one"
                                        }
                                    },
                                    {
                                        "targetConfigured": {
                                            "label": "//buildscripts/smoke_tests:helper"
                                        }
                                    },
                                    {
                                        "targetConfigured": {
                                            "label": "//buildscripts/smoke_tests:skipped"
                                        }
                                    },
                                ],
                            }
                        ),
                        json.dumps(
                            {
                                "id": {
                                    "targetConfigured": {"label": "//buildscripts/smoke_tests:one"}
                                },
                                "configured": {"targetKind": "py_test rule"},
                            }
                        ),
                        json.dumps(
                            {
                                "id": {
                                    "targetConfigured": {
                                        "label": "//buildscripts/smoke_tests:skipped"
                                    }
                                },
                                "configured": {"targetKind": "py_test rule"},
                            }
                        ),
                        json.dumps(
                            {
                                "id": {
                                    "targetConfigured": {
                                        "label": "//buildscripts/smoke_tests:helper"
                                    }
                                },
                                "configured": {"targetKind": "genrule rule"},
                            }
                        ),
                        json.dumps(
                            {
                                "id": {
                                    "targetCompleted": {
                                        "label": "//buildscripts/smoke_tests:skipped"
                                    }
                                },
                                "aborted": {"reason": "SKIPPED"},
                            }
                        ),
                    ]
                ),
                encoding="utf-8",
            )
            plan = hermetic_container_integration.MacOSCrossHostTestPlan(
                build_args=["build", "//..."],
                startup_args=[],
                host_test_options=[],
                target_patterns=["//..."],
                test_args=[],
                test_env={},
                test_tag_filters=[],
                build_event_json_file="build_events.json",
                runs_per_test=1,
                run_host_tests=True,
            )

            with mock.patch.object(hermetic_container_integration.subprocess, "run") as run:
                labels = hermetic_container_integration._expand_host_test_labels(
                    "/tmp/bazel",
                    plan,
                    {},
                    repo_root=repo_root,
                )

        self.assertEqual(labels, ["//buildscripts/smoke_tests:one"])
        run.assert_not_called()

    def test_host_test_query_expression_applies_exact_tag_filters(self):
        expression = hermetic_container_integration._host_test_query_expression(
            ["//src/mongo/bson/util/..."],
            ["mongo_unittest", "-requires_large_host"],
        )

        self.assertEqual(
            expression,
            (
                '(attr("tags", "(^|\\\\[|, )mongo_unittest($|,|\\\\])", '
                "tests(set(//src/mongo/bson/util/...))) except "
                'attr("tags", "(^|\\\\[|, )requires_large_host($|,|\\\\])", '
                'attr("tags", "(^|\\\\[|, )mongo_unittest($|,|\\\\])", '
                "tests(set(//src/mongo/bson/util/...)))))"
            ),
        )

    def test_label_to_host_executable(self):
        self.assertEqual(
            hermetic_container_integration._label_to_host_executable(
                "//src/mongo/base:status_test",
                pathlib.Path("/repo"),
            ),
            pathlib.Path("/repo/bazel-bin/src/mongo/base/status_test"),
        )
        self.assertEqual(
            hermetic_container_integration._label_to_host_executable(
                "//:root_test", pathlib.Path("/repo")
            ),
            pathlib.Path("/repo/bazel-bin/root_test"),
        )
        self.assertEqual(
            hermetic_container_integration._label_to_host_executable(
                "//src/mongo/base:status_test",
                pathlib.Path("/repo"),
                executable_suffix=".exe",
            ),
            pathlib.Path("/repo/bazel-bin/src/mongo/base/status_test.exe"),
        )

    def test_resmoke_deps_path_file(self):
        self.assertEqual(
            hermetic_container_integration._resmoke_deps_path_file(
                "//buildscripts/bazel_testbuilds:jstest_timeout",
                pathlib.Path("/repo"),
            ),
            pathlib.Path(
                "/repo/bazel-bin/buildscripts/bazel_testbuilds/"
                "jstest_timeout_resmoke_deps_path.txt"
            ),
        )

    def test_runs_host_test_executable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            executable = repo_root / "bazel-bin" / "src" / "mongo" / "ok_test"
            executable.parent.mkdir(parents=True)
            executable.write_text(
                "#!/bin/sh\n"
                'test "$FROM_ARG" = "value" || exit 7\n'
                'test "$TEST_TARGET" = "//src/mongo:ok_test" || exit 10\n'
                'test "$TEST_BINARY" = "$PWD/bazel-bin/src/mongo/ok_test" || exit 11\n'
                'test "$1" = "--fileNameFilter" || exit 8\n'
                'test "$2" = "ok_test" || exit 9\n',
                encoding="utf-8",
            )
            executable.chmod(0o755)
            plan = hermetic_container_integration.MacOSCrossHostTestPlan(
                build_args=["build", "//src/mongo:ok_test"],
                startup_args=[],
                host_test_options=[],
                target_patterns=["//src/mongo:ok_test"],
                test_args=["--fileNameFilter", "ok_test"],
                test_env={"FROM_ARG": "value"},
                test_tag_filters=[],
                build_event_json_file=None,
                runs_per_test=1,
                run_host_tests=True,
            )

            with mock.patch.object(
                hermetic_container_integration,
                "_expand_host_test_labels",
                return_value=["//src/mongo:ok_test"],
            ):
                rc = hermetic_container_integration._run_macos_cross_host_tests(
                    "/tmp/bazel",
                    plan,
                    {},
                    repo_root=repo_root,
                )

        self.assertEqual(rc, 0)

    def test_runs_resmoke_tests_through_one_host_bazel_invocation(self):
        label = "//buildscripts/bazel_testbuilds:jstest_timeout"
        second_label = "//buildscripts/smoke_tests:core"

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            cross_bin_root = repo_root / "cross-bin"
            cross_bin_root.mkdir()
            (repo_root / "bazel-bin").symlink_to(cross_bin_root, target_is_directory=True)
            deps_file = hermetic_container_integration._resmoke_deps_path_file(
                label, repo_root, cross_bin_root
            )
            deps_file.parent.mkdir(parents=True)
            deps_file.write_text(
                "bazel-out/aarch64-fastbuild/bin/src/mongo/shell/mongo\n"
                "bazel-out/aarch64-fastbuild/bin/src/mongo/db/mongod\n",
                encoding="utf-8",
            )
            second_deps_file = hermetic_container_integration._resmoke_deps_path_file(
                second_label, repo_root, cross_bin_root
            )
            second_deps_file.parent.mkdir(parents=True)
            second_deps_file.write_text(
                "bazel-out/aarch64-fastbuild/bin/src/mongo/shell/mongo\n"
                "bazel-out/aarch64-fastbuild/bin/src/mongo/db/mongos\n",
                encoding="utf-8",
            )
            plan = hermetic_container_integration.MacOSCrossHostTestPlan(
                build_args=["build", label],
                startup_args=["--output_base=/tmp/host-bazel"],
                host_test_options=["--test_arg=jstests/core/smoke.js"],
                target_patterns=[label],
                test_args=[],
                test_env={},
                test_tag_filters=[],
                build_event_json_file=None,
                runs_per_test=1,
                run_host_tests=True,
            )

            completed = subprocess.CompletedProcess(args=[], returncode=0)
            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_expand_host_test_labels",
                    return_value=[label, second_label],
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    return_value=completed,
                ) as run,
            ):
                rc = hermetic_container_integration._run_macos_cross_host_tests(
                    "/tmp/bazel",
                    plan,
                    {
                        "MONGO_BAZEL_IN_HERMETIC_CONTAINER": "1",
                        "MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1",
                    },
                    repo_root=repo_root,
                )

            self.assertEqual(rc, 0)
            run.assert_called_once()
            host_args = run.call_args.args[0]
            self.assertEqual(
                host_args[:7],
                [
                    "/tmp/bazel",
                    "--output_base=/tmp/host-bazel",
                    "test",
                    "--//bazel/resmoke:skip_deps_for_cquery=True",
                    "--local_resources=cpu=HOST_CPUS",
                    "--local_test_jobs=HOST_CPUS",
                    "--test_arg=jstests/core/smoke.js",
                ],
            )
            self.assertEqual(host_args[-2:], [label, second_label])
            deps_map_arg = host_args[-3]
            self.assertTrue(deps_map_arg.startswith("--test_env=DEPS_PATH_MAP_FILE="))
            deps_map_file = pathlib.Path(deps_map_arg.split("=", 2)[2])
            self.assertEqual(
                json.loads(deps_map_file.read_text(encoding="utf-8")),
                {
                    label: os.pathsep.join(
                        [
                            str((cross_bin_root / "src/mongo/shell/mongo").resolve()),
                            str((cross_bin_root / "src/mongo/db/mongod").resolve()),
                        ]
                    ),
                    second_label: os.pathsep.join(
                        [
                            str((cross_bin_root / "src/mongo/shell/mongo").resolve()),
                            str((cross_bin_root / "src/mongo/db/mongos").resolve()),
                        ]
                    ),
                },
            )
            self.assertEqual(run.call_args.kwargs["env"]["MONGO_BAZEL_USE_HERMETIC_CONTAINER"], "0")
            self.assertEqual(run.call_args.kwargs["env"]["MONGO_MACOS_CROSS_DEFAULT_CONFIG"], "0")
            self.assertNotIn("MONGO_BAZEL_IN_HERMETIC_CONTAINER", run.call_args.kwargs["env"])

    def test_runs_host_binary_executable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            executable = repo_root / "bazel-bin" / "src" / "mongo" / "ok_test"
            executable.parent.mkdir(parents=True)
            executable.write_text(
                "#!/bin/sh\n" 'printf \'%s:%s\' "$1" "$2" > "$PWD/out.txt"\n',
                encoding="utf-8",
            )
            executable.chmod(0o755)
            plan = hermetic_container_integration.MacOSCrossHostRunPlan(
                build_args=[],
                target="//src/mongo:ok_test",
                run_args=["1", "2"],
            )

            rc = hermetic_container_integration._run_macos_cross_host_binary(plan, repo_root)
            output = (repo_root / "out.txt").read_text(encoding="utf-8")

        self.assertEqual(rc, 0)
        self.assertEqual(output, "1:2")

    def test_host_test_failure_returns_nonzero(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            executable = repo_root / "bazel-bin" / "src" / "mongo" / "bad_test"
            executable.parent.mkdir(parents=True)
            executable.write_text("#!/bin/sh\nexit 42\n", encoding="utf-8")
            executable.chmod(0o755)
            plan = hermetic_container_integration.MacOSCrossHostTestPlan(
                build_args=["build", "//src/mongo:bad_test"],
                startup_args=[],
                host_test_options=[],
                target_patterns=["//src/mongo:bad_test"],
                test_args=[],
                test_env={},
                test_tag_filters=[],
                build_event_json_file=None,
                runs_per_test=1,
                run_host_tests=True,
            )

            with mock.patch.object(
                hermetic_container_integration,
                "_expand_host_test_labels",
                return_value=["//src/mongo:bad_test"],
            ):
                rc = hermetic_container_integration._run_macos_cross_host_tests(
                    "/tmp/bazel",
                    plan,
                    {},
                    repo_root=repo_root,
                )

        self.assertEqual(rc, 1)

    def test_runs_windows_cross_host_binary(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            executable = repo_root / "bazel-bin" / "src" / "mongo" / "ok_test.exe"
            executable.parent.mkdir(parents=True)
            executable.write_text("", encoding="utf-8")
            plan = hermetic_container_integration.MacOSCrossHostRunPlan(
                build_args=["build", "//src/mongo:ok_test"],
                target="//src/mongo:ok_test",
                run_args=["--fileNameFilter", "ok_test"],
            )

            with mock.patch.object(
                hermetic_container_integration.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(args=[], returncode=0),
            ) as run:
                rc = hermetic_container_integration._run_windows_cross_host_binary(
                    plan, repo_root=repo_root
                )

        self.assertEqual(rc, 0)
        run.assert_called_once()
        self.assertEqual(
            run.call_args.args[0],
            [str(executable), "--fileNameFilter", "ok_test"],
        )


if __name__ == "__main__":
    unittest.main()
