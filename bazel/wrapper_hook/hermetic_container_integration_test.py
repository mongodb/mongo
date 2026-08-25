"""Unit tests for bazel.wrapper_hook.hermetic_container_integration."""

import hashlib
import importlib.util
import json
import ntpath
import os
import pathlib
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import BytesIO, StringIO
from unittest import mock

from bazel.wrapper_hook import hermetic_container_integration

HERMETIC_CONTAINER_PATH = (
    pathlib.Path(__file__).parents[1] / "hermetic_container" / "hermetic_container.py"
)
HERMETIC_CONTAINER_SPEC = importlib.util.spec_from_file_location(
    "hermetic_container_under_test", HERMETIC_CONTAINER_PATH
)
assert HERMETIC_CONTAINER_SPEC is not None
hermetic_container = importlib.util.module_from_spec(HERMETIC_CONTAINER_SPEC)
assert HERMETIC_CONTAINER_SPEC.loader is not None
HERMETIC_CONTAINER_SPEC.loader.exec_module(hermetic_container)

MACOS_CROSS_ACTION_WRAPPER_PATH = (
    pathlib.Path(__file__).parents[1]
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


class HermeticContainerWorkspaceDiscoveryTest(unittest.TestCase):
    def test_finds_all_supported_workspace_markers(self):
        for marker in hermetic_container.BAZEL_WORKSPACE_FILES:
            with self.subTest(marker=marker), tempfile.TemporaryDirectory() as temp_dir:
                workspace = pathlib.Path(temp_dir) / "workspace"
                nested_directory = workspace / "nested"
                nested_directory.mkdir(parents=True)
                (workspace / marker).touch()

                with mock.patch.dict(
                    hermetic_container.os.environ,
                    {"HERMETIC_CONTAINER_DIRECTORY": str(nested_directory)},
                    clear=True,
                ):
                    self.assertEqual(
                        hermetic_container.DockerInstance._find_workspace_directory(),
                        str(workspace),
                    )

    def test_rejects_directory_without_workspace_marker(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            with mock.patch.dict(
                hermetic_container.os.environ,
                {"HERMETIC_CONTAINER_DIRECTORY": temp_dir},
                clear=True,
            ):
                with self.assertRaisesRegex(FileNotFoundError, "No Bazel workspace file"):
                    hermetic_container.DockerInstance._find_workspace_directory()


class HermeticContainerCommandTest(unittest.TestCase):
    @staticmethod
    def _docker_instance() -> object:
        instance = hermetic_container.DockerInstance.__new__(hermetic_container.DockerInstance)
        instance.docker_command = "docker"
        instance.docker_run_privileged = False
        instance.user = ""
        instance.instance_name = "mongo-hermetic-container"
        instance.command = "/usr/bin/bazel"
        instance.bazel_rc_file = "/tmp/bazelrc"
        instance.bazel_output_base = "/tmp/bazel output"
        instance.bazel_user_output_root = ""
        instance.docker_machine = None
        return instance

    def test_send_command_uses_argv_and_preserves_bazel_arguments(self):
        instance = self._docker_instance()
        bazel_args = [
            "build",
            "//src/mongo:target with spaces",
            "--define=unsafe=$(touch should-not-run); echo should-not-run",
        ]
        with (
            mock.patch.object(
                hermetic_container.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(args=[], returncode=37),
            ) as run,
            mock.patch.object(hermetic_container.os, "system") as system,
        ):
            self.assertEqual(
                instance.send_command(bazel_args, bazel_rc_file="/tmp/invocation.bazelrc"), 37
            )

        system.assert_not_called()
        run.assert_called_once()
        command = run.call_args.args[0]
        bazel_index = command.index("/usr/bin/bazel")
        self.assertEqual(
            command[bazel_index:],
            [
                "/usr/bin/bazel",
                "--bazelrc=/tmp/bazelrc",
                "--bazelrc=/tmp/invocation.bazelrc",
                "--output_user_root=%s" % hermetic_container.TEMP_BAZEL_OUTPUT_USER_ROOT,
                "--output_base=/tmp/bazel output",
                *bazel_args,
            ],
        )
        self.assertEqual(run.call_args.kwargs, {"check": False, "env": None})

    def test_network_commands_retry_transient_failures(self):
        instance = self._docker_instance()
        with (
            mock.patch.object(hermetic_container.subprocess, "call", side_effect=[1, 0]) as run,
            mock.patch.object(hermetic_container.time, "sleep") as sleep,
        ):
            self.assertEqual(instance._run_silent_command("docker pull image", retry=True), 0)

        self.assertEqual(run.call_count, 2)
        sleep.assert_called_once_with(hermetic_container.CONTAINER_NETWORK_RETRY_DELAY_SECONDS)

    def test_user_output_base_tracks_bazels_md5_workspace_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            workspace = root / "workspace"
            output_root = root / "output-root"
            workspace.mkdir()

            instance = hermetic_container.DockerInstance(
                instance_name="container",
                image_name="image",
                run_command="/bin/bash",
                docker_command="docker",
                dockerfile="",
                repository="",
                directory=str(workspace),
                command="",
                volumes=[],
                ports=[],
                env_vars=[],
                gpus=[],
                network="",
                run_deps=[],
                docker_compose_file="",
                docker_compose_command="docker-compose",
                docker_compose_project_name="project",
                docker_compose_services="",
                bazel_user_output_root=str(output_root),
                bazel_rc_file="",
                docker_run_privileged=False,
                docker_machine=None,
                hermetic_container_run_file="",
                workspace_hex=True,
                delegated_volume=False,
                user="1:1",
                docker_build_args="",
                shm_size="",
                platform="",
            )

            sha256_digest = hashlib.sha256(str(workspace).encode()).hexdigest()
            md5_digest = hashlib.md5(  # nosemgrep: insecure-hash-algorithm-md5
                str(workspace).encode(), usedforsecurity=False
            ).hexdigest()
            self.assertEqual(instance.workspace_hex_digest, sha256_digest)
            self.assertEqual(instance.bazel_output_base_digest, md5_digest)
            self.assertEqual(instance.bazel_output_base, str(output_root / md5_digest))
            self.assertTrue((output_root / md5_digest).is_dir())

    def test_windows_symlink_fix_includes_tmp_convenience_links(self):
        instance = self._docker_instance()
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            convenience_dir = repo_root / ".tmp"
            target = repo_root / "output"
            convenience_dir.mkdir()
            target.mkdir()
            (convenience_dir / "bazel-bin").symlink_to(target, target_is_directory=True)
            instance.directory = str(repo_root)

            with mock.patch.object(
                hermetic_container.subprocess,
                "check_output",
                return_value=b"/workspace/output\n",
            ) as check_output:
                instance._fix_win_symlink(["docker", "exec", "container"])

            check_output.assert_called_once_with(
                ["docker", "exec", "container", "realpath", ".tmp/bazel-bin"],
                env=None,
            )

    def test_docker_machine_environment_is_parsed_without_a_shell(self):
        instance = self._docker_instance()
        instance.docker_machine = "cross-builder"
        docker_machine_output = (
            'export DOCKER_TLS_VERIFY="1"\n'
            'export DOCKER_HOST="tcp://127.0.0.1:2376"\n'
            'export DOCKER_MACHINE_NAME="cross-builder"\n'
        )
        with (
            mock.patch.object(instance, "_command_exists", return_value=True),
            mock.patch.object(
                hermetic_container.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    args=[],
                    returncode=0,
                    stdout=docker_machine_output,
                    stderr="",
                ),
            ) as run,
        ):
            environment = instance._docker_machine_environment()

        self.assertEqual(environment["DOCKER_TLS_VERIFY"], "1")
        self.assertEqual(environment["DOCKER_HOST"], "tcp://127.0.0.1:2376")
        self.assertEqual(environment["DOCKER_MACHINE_NAME"], "cross-builder")
        run.assert_called_once_with(
            ["docker-machine", "env", "--shell", "bash", "cross-builder"],
            check=False,
            capture_output=True,
            text=True,
        )


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


class LinuxBazelDownloadTest(unittest.TestCase):
    def test_download_is_verified_and_atomically_replaces_the_cache(self):
        payload = b"new Linux Bazel"
        expected_sha256 = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as temp_dir:
            destination = pathlib.Path(temp_dir) / "bin" / "bazel"
            destination.parent.mkdir()
            destination.write_bytes(b"previous Linux Bazel")

            with mock.patch.object(
                hermetic_container_integration.urllib.request,
                "urlopen",
                return_value=BytesIO(payload),
            ):
                hermetic_container_integration._download_file(
                    "https://example.invalid/bazel",
                    destination,
                    expected_sha256,
                )

            self.assertEqual(destination.read_bytes(), payload)
            self.assertEqual(
                hermetic_container_integration._linux_bazel_checksum_file(destination).read_text(
                    encoding="utf-8"
                ),
                f"{expected_sha256}\n",
            )
            self.assertTrue(hermetic_container_integration._linux_bazel_cache_is_valid(destination))

    def test_checksum_mismatch_leaves_the_existing_cache_untouched(self):
        expected_sha256 = hashlib.sha256(b"expected Linux Bazel").hexdigest()
        old_contents = b"previous Linux Bazel"
        with tempfile.TemporaryDirectory() as temp_dir:
            destination = pathlib.Path(temp_dir) / "bin" / "bazel"
            destination.parent.mkdir()
            destination.write_bytes(old_contents)

            with (
                mock.patch.object(
                    hermetic_container_integration.urllib.request,
                    "urlopen",
                    return_value=BytesIO(b"corrupt Linux Bazel"),
                ),
                self.assertRaisesRegex(RuntimeError, "does not match the expected SHA-256"),
            ):
                hermetic_container_integration._download_file(
                    "https://example.invalid/bazel",
                    destination,
                    expected_sha256,
                )

            self.assertEqual(destination.read_bytes(), old_contents)
            self.assertFalse(
                hermetic_container_integration._linux_bazel_checksum_file(destination).exists()
            )
            self.assertEqual(list(destination.parent.glob(".bazel.*.tmp")), [])

    def test_resolve_container_bazel_reuses_only_a_verified_cache_entry(self):
        payload = b"cached Linux Bazel"
        expected_sha256 = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            repo_root.joinpath(".bazelversion").write_text("7.5.0\n", encoding="utf-8")
            destination = (
                hermetic_container_integration._hermetic_container_state_dir(repo_root)
                / "bazel"
                / "bazel-7.5.0-linux-x86_64"
                / "bin"
                / "bazel"
            )
            destination.parent.mkdir(parents=True)
            destination.write_bytes(payload)
            hermetic_container_integration._linux_bazel_checksum_file(destination).write_text(
                f"{expected_sha256}\n",
                encoding="utf-8",
            )

            with mock.patch.object(
                hermetic_container_integration.urllib.request,
                "urlopen",
            ) as urlopen:
                command, mounted_binary = hermetic_container_integration._resolve_container_bazel(
                    pathlib.Path("/usr/bin/bazel"),
                    {"MONGO_HERMETIC_CONTAINER_CONTAINER_ARCH": "x86_64"},
                    repo_root,
                    "Darwin",
                )

            self.assertEqual(command, str(destination))
            self.assertEqual(mounted_binary, destination)
            urlopen.assert_not_called()


class HermeticContainerEnablementTest(unittest.TestCase):
    def setUp(self):
        self.container_detection = mock.patch.object(
            hermetic_container_integration, "_is_running_in_container", return_value=False
        )
        self.container_detection.start()
        self.addCleanup(self.container_detection.stop)

    def test_default_enabled_on_linux_when_docker_exists(self):
        for machine in ["x86_64", "aarch64"]:
            with self.subTest(machine=machine):
                self.assertTrue(
                    hermetic_container_integration.should_use_hermetic_container(
                        env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                        system="Linux",
                        docker_exists=lambda: True,
                        machine=machine,
                    )
                )
                self.assertEqual(
                    hermetic_container_integration.select_integration_mode(
                        env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                        system="Linux",
                        docker_exists=lambda: True,
                        args=["build", "install-dist-test"],
                        machine=machine,
                    ),
                    hermetic_container_integration.IntegrationMode.LINUX_HOST_CONTAINER,
                )

    def test_linux_s390x_and_ppc64le_use_host_container_for_local_builds(self):
        for machine in ["s390x", "ppc64le"]:
            with self.subTest(machine=machine):
                self.assertTrue(
                    hermetic_container_integration.should_use_hermetic_container(
                        env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                        system="Linux",
                        docker_exists=lambda: True,
                        machine=machine,
                    )
                )
                self.assertEqual(
                    hermetic_container_integration.select_integration_mode(
                        env={
                            "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
                            "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                        },
                        system="Linux",
                        docker_exists=lambda: True,
                        args=["build", "install-dist-test"],
                        machine=machine,
                    ),
                    hermetic_container_integration.IntegrationMode.LINUX_HOST_CONTAINER,
                )

    def test_linux_s390x_and_ppc64le_cross_rbe_uses_host_bazel(self):
        for config, machine in [
            ("linux-s390x-cross-rbe", "s390x"),
            ("linux-ppc64le-cross-rbe", "ppc64le"),
        ]:
            with self.subTest(config=config, machine=machine):
                self.assertEqual(
                    hermetic_container_integration.select_integration_mode(
                        env={},
                        system="Linux",
                        docker_exists=lambda: True,
                        args=["build", f"--config={config}", "install-dist-test"],
                        machine=machine,
                    ),
                    hermetic_container_integration.IntegrationMode.LINUX_CROSS_HOST_RBE,
                )

    def test_linux_cross_rbe_can_be_disabled(self):
        self.assertEqual(
            hermetic_container_integration.select_integration_mode(
                env={"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "0"},
                system="Linux",
                docker_exists=lambda: True,
                args=["build", "--config=linux-s390x-cross-rbe", "install-dist-test"],
                machine="s390x",
            ),
            hermetic_container_integration.IntegrationMode.DIRECT,
        )

    def test_native_toolchain_disables_linux_host_container(self):
        self.assertEqual(
            hermetic_container_integration.select_integration_mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                system="Linux",
                docker_exists=lambda: True,
                args=["build", "--config=native_toolchain", "install-dist-test"],
                machine="aarch64",
            ),
            hermetic_container_integration.IntegrationMode.DIRECT,
        )

    def test_native_toolchain_config_aliases_are_loaded_from_workspace_rc(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir) / "repo"
            repo_root.mkdir()
            (repo_root / ".bazelrc").write_text(
                "try-import %workspace%/.bazelrc.local\n", encoding="utf-8"
            )
            (repo_root / ".bazelrc.local").write_text(
                "common:host-native --config=native_toolchain\n", encoding="utf-8"
            )

            mode = hermetic_container_integration.select_integration_mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                system="Linux",
                docker_exists=lambda: True,
                args=["--nosystem_rc", "--nohome_rc", "build", "--config=host-native"],
                machine="x86_64",
                repo_root=repo_root,
            )

        self.assertEqual(mode, hermetic_container_integration.IntegrationMode.DIRECT)

    def test_native_toolchain_config_alias_is_loaded_from_home_rc(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir) / "repo"
            repo_root.mkdir()
            home = pathlib.Path(temp_dir) / "home"
            home.mkdir()
            (home / ".bazelrc").write_text(
                "common:host-native --config=native_toolchain\n", encoding="utf-8"
            )

            mode = hermetic_container_integration.select_integration_mode(
                env={
                    "HOME": str(home),
                    "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                },
                system="Linux",
                docker_exists=lambda: True,
                args=["--nosystem_rc", "--noworkspace_rc", "build", "--config=host-native"],
                machine="x86_64",
                repo_root=repo_root,
            )

        self.assertEqual(mode, hermetic_container_integration.IntegrationMode.DIRECT)

    def test_windows_build_defaults_to_native_toolchain(self):
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: True,
                args=["build", "install-dist-test"],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1"},
                system="Windows",
                docker_exists=lambda: True,
                args=["build", "install-dist-test"],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: False,
                args=["build", "install-dist-test"],
            )
        )

    def test_windows_build_can_opt_in_to_cross_hermetic_container(self):
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_WINDOWS_CROSS_DEFAULT_CONFIG": "1"},
                system="Windows",
                docker_exists=lambda: True,
                args=["build", "install-dist-test"],
            )
        )

    def test_windows_non_cross_commands_stay_local(self):
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: True,
                args=["info"],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1"},
                system="Windows",
                docker_exists=lambda: True,
                args=["info"],
            )
        )

    def test_windows_run_defaults_to_native_toolchain_for_test_targets(self):
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: True,
                args=["run", "//src/mongo/stdx:stdx_test"],
            )
        )
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_WINDOWS_CROSS_DEFAULT_CONFIG": "1"},
                system="Windows",
                docker_exists=lambda: True,
                args=["run", "//src/mongo/stdx:stdx_test"],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: True,
                args=["run", "format"],
            )
        )

    def test_windows_test_does_not_default_to_cross_hermetic_container(self):
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: True,
                args=["test", "//src/mongo/stdx:stdx_test"],
            )
        )

    def test_windows_cross_default_config_requires_opt_in(self):
        args = hermetic_container_integration._bazel_args_with_default_windows_cross_config(
            ["build", "install-dist-test"],
            {},
            system="Windows",
        )

        self.assertEqual(args, ["build", "install-dist-test"])
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: True,
                args=["build", "install-dist-test"],
            )
        )
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Windows",
                docker_exists=lambda: True,
                args=[
                    "run",
                    f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}",
                    "+stdx_test",
                ],
            )
        )

    def test_windows_cross_default_config_preserves_explicit_platforms(self):
        args = hermetic_container_integration._bazel_args_with_default_windows_cross_config(
            ["build", "--platforms=//bazel/platforms:windows_amd64", "install-dist-test"],
            {"MONGO_WINDOWS_CROSS_DEFAULT_CONFIG": "1"},
            system="Windows",
        )

        self.assertEqual(
            args,
            ["build", "--platforms=//bazel/platforms:windows_amd64", "install-dist-test"],
        )

    def test_windows_cross_default_config_injects_after_command(self):
        args = hermetic_container_integration._bazel_args_with_default_windows_cross_config(
            ["--output_base=C:/tmp/bazel", "build", "install-dist-test"],
            {"MONGO_WINDOWS_CROSS_DEFAULT_CONFIG": "1"},
            system="Windows",
        )

        self.assertEqual(
            args,
            [
                "--output_base=C:/tmp/bazel",
                "build",
                f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}",
                "install-dist-test",
            ],
        )

    def test_macos_uses_hermetic_container_for_cross_config_or_explicit_enablement(self):
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: True,
                args=["build", "//src/mongo:target"],
            )
        )
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: True,
                args=["build", "--config=macos-cross-arm64", "//src/mongo:target"],
            )
        )
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1"},
                system="Darwin",
                docker_exists=lambda: True,
                args=["build", "//src/mongo:target"],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: True,
                args=["clean"],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: True,
                args=["run", "format"],
            )
        )

    def test_macos_cross_test_routes_through_integration_without_docker(self):
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: False,
                args=[
                    "test",
                    "--config=macos-cross-arm64",
                    "--config=remote_link",
                    "//src/mongo:target_test",
                ],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: False,
                args=["test", "--config=remote_link", "//src/mongo:target_test"],
            )
        )
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: False,
                args=["test", "--config=macos-cross-arm64", "//src/mongo:target_test"],
            )
        )

    def test_macos_cross_default_config_uses_host_arch(self):
        self.assertEqual(
            hermetic_container_integration._bazel_args_with_default_macos_cross_config(
                ["test", "+some_test"],
                {"MONGO_MACOS_CROSS_DEFAULT_CONFIG": "1"},
                system="Darwin",
                machine="arm64",
            ),
            ["test", "--config=macos-cross-arm64", "+some_test"],
        )
        self.assertEqual(
            hermetic_container_integration._bazel_args_with_default_macos_cross_config(
                ["build", "install-dist-test"],
                {"MONGO_MACOS_CROSS_DEFAULT_CONFIG": "1"},
                system="Darwin",
                machine="x86_64",
            ),
            ["build", "--config=macos-cross-x86_64", "install-dist-test"],
        )
        self.assertEqual(
            hermetic_container_integration._bazel_args_with_default_macos_cross_config(
                ["run", "format"],
                {},
                system="Darwin",
                machine="arm64",
            ),
            ["run", "format"],
        )
        self.assertEqual(
            hermetic_container_integration._bazel_args_with_default_macos_cross_config(
                ["clean"],
                {},
                system="Darwin",
                machine="arm64",
            ),
            ["clean"],
        )
        self.assertEqual(
            hermetic_container_integration._bazel_args_with_default_macos_cross_config(
                ["run", "//src/mongo/stdx:stdx_test", "--", "--fileNameFilter", "stdx_test"],
                {"MONGO_MACOS_CROSS_DEFAULT_CONFIG": "1"},
                system="Darwin",
                machine="arm64",
            ),
            [
                "run",
                "--config=macos-cross-arm64",
                "//src/mongo/stdx:stdx_test",
                "--",
                "--fileNameFilter",
                "stdx_test",
            ],
        )

    def test_macos_cross_default_config_requires_opt_in(self):
        args = hermetic_container_integration._bazel_args_with_default_macos_cross_config(
            ["test", "+some_test"],
            {},
            system="Darwin",
            machine="arm64",
        )

        self.assertEqual(args, ["test", "+some_test"])
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: True,
                args=["test", "+some_test"],
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={},
                system="Darwin",
                docker_exists=lambda: True,
                args=["clean"],
            )
        )

    def test_env_can_disable_hermetic_container(self):
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "0"},
                system="Linux",
                docker_exists=lambda: True,
            )
        )
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_BAZEL_USE_HERMETIC_CONTAINER": "0"},
                system="Darwin",
                docker_exists=lambda: True,
            )
        )

    def test_skips_inside_hermetic_container(self):
        self.assertFalse(
            hermetic_container_integration.should_use_hermetic_container(
                env={"MONGO_BAZEL_IN_HERMETIC_CONTAINER": "1"},
                system="Linux",
                docker_exists=lambda: True,
            )
        )

    def test_defaults_to_native_when_already_in_a_container(self):
        with mock.patch.object(
            hermetic_container_integration, "_is_running_in_container", return_value=True
        ):
            self.assertEqual(
                hermetic_container_integration.select_integration_mode(
                    env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                    system="Linux",
                    docker_exists=lambda: True,
                    args=["build", "install-dist-test"],
                    machine="x86_64",
                ),
                hermetic_container_integration.IntegrationMode.DIRECT,
            )

    def test_can_explicitly_enable_nested_container(self):
        with mock.patch.object(
            hermetic_container_integration, "_is_running_in_container", return_value=True
        ):
            self.assertEqual(
                hermetic_container_integration.select_integration_mode(
                    env={
                        "MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1",
                        "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                    },
                    system="Linux",
                    docker_exists=lambda: True,
                    args=["build", "install-dist-test"],
                    machine="x86_64",
                ),
                hermetic_container_integration.IntegrationMode.LINUX_HOST_CONTAINER,
            )

    def test_ci_uses_default_linux_container_mode(self):
        for distro, machine in [
            ("amazon_linux_2023", "aarch64"),
            ("rhel9", "x86_64"),
        ]:
            with self.subTest(distro=distro, machine=machine):
                env = {"CI": "1", "MONGO_HERMETIC_CONTAINER_DISTRO": distro}
                self.assertTrue(
                    hermetic_container_integration.should_use_hermetic_container(
                        env=env,
                        system="Linux",
                        docker_exists=lambda: True,
                        machine=machine,
                    )
                )
                self.assertEqual(
                    hermetic_container_integration.select_integration_mode(
                        env=env,
                        system="Linux",
                        docker_exists=lambda: True,
                        args=["build", "install-dist-test"],
                        machine=machine,
                    ),
                    hermetic_container_integration.IntegrationMode.LINUX_HOST_CONTAINER,
                )

    def test_dry_run_does_not_require_docker(self):
        self.assertTrue(
            hermetic_container_integration.should_use_hermetic_container(
                env={
                    "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
                    "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                },
                system="Linux",
                docker_exists=lambda: False,
                machine="x86_64",
            )
        )


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


class LinuxHostContainerTest(unittest.TestCase):
    def _mode(self, env=None, args=(), machine="x86_64", docker=True):
        return hermetic_container_integration.select_integration_mode(
            env=env or {},
            system="Linux",
            docker_exists=lambda: docker,
            args=list(args),
            machine=machine,
        )

    def test_injected_command_options_precede_user_options(self):
        args = hermetic_container_integration._append_bazel_command_options(
            [
                "build",
                "//src/mongo:target",
                "--strategy=CppCompile=local",
                "--local_resources=cpu=HOST_CPUS*.5",
                "--",
                "user-argument",
            ],
            [
                "--strategy=CppCompile=remote",
                "--local_resources=cpu=HOST_CPUS",
            ],
        )

        self.assertEqual(
            args,
            [
                "build",
                "--strategy=CppCompile=remote",
                "--local_resources=cpu=HOST_CPUS",
                "//src/mongo:target",
                "--strategy=CppCompile=local",
                "--local_resources=cpu=HOST_CPUS*.5",
                "--",
                "user-argument",
            ],
        )

    def test_opt_out_env_builds_like_normal(self):
        for value in ["0", "false", "no", "off"]:
            with self.subTest(value=value):
                self.assertEqual(
                    self._mode(
                        env={
                            "MONGO_LINUX_CONTAINER_ACTIONS": value,
                            "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                        },
                        args=["build", "install-dist-test"],
                    ),
                    hermetic_container_integration.IntegrationMode.DIRECT,
                )

    def test_undetectable_distro_builds_like_normal(self):
        stderr = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration, "detect_host_distro", return_value=None
            ),
            redirect_stderr(stderr),
        ):
            self.assertEqual(
                self._mode(args=["build", "install-dist-test"]),
                hermetic_container_integration.IntegrationMode.DIRECT,
            )
        self.assertIn("WARNING:", stderr.getvalue())
        self.assertIn("host distro could not be detected", stderr.getvalue())

    def test_distro_without_toolchain_builds_like_normal(self):
        # ubuntu24 has a pinned container but no s390x toolchain.
        stderr = StringIO()
        with redirect_stderr(stderr):
            mode = self._mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "ubuntu24"},
                args=["build", "install-dist-test"],
                machine="s390x",
            )
        self.assertEqual(
            mode,
            hermetic_container_integration.IntegrationMode.DIRECT,
        )
        self.assertIn("WARNING:", stderr.getvalue())
        self.assertIn("ubuntu24", stderr.getvalue())
        self.assertIn("s390x", stderr.getvalue())

    def test_unsupported_architecture_builds_like_normal_with_warning(self):
        stderr = StringIO()
        with redirect_stderr(stderr):
            mode = self._mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                args=["build", "install-dist-test"],
                machine="mips64",
            )
        self.assertEqual(
            mode,
            hermetic_container_integration.IntegrationMode.DIRECT,
        )
        self.assertIn("WARNING:", stderr.getvalue())
        self.assertIn("mips64", stderr.getvalue())

    def test_detected_distro_enables_host_container_mode(self):
        with mock.patch.object(
            hermetic_container_integration, "detect_host_distro", return_value="rhel9"
        ):
            self.assertEqual(
                self._mode(args=["test", "+stdx_test"]),
                hermetic_container_integration.IntegrationMode.LINUX_HOST_CONTAINER,
            )

    def test_clean_runs_directly_without_native_fallback_warning(self):
        stderr = StringIO()
        with redirect_stderr(stderr):
            mode = self._mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                args=["clean"],
            )

        self.assertEqual(mode, hermetic_container_integration.IntegrationMode.DIRECT)
        self.assertNotIn("WARNING:", stderr.getvalue())

    def test_query_uses_host_container_mode(self):
        self.assertEqual(
            self._mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                args=["query", "//src/mongo/..."],
            ),
            hermetic_container_integration.IntegrationMode.LINUX_HOST_CONTAINER,
        )

    def test_macos_cross_config_uses_normal_setup_on_linux(self):
        self.assertEqual(
            self._mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023"},
                args=["build", "--config=macos-cross-arm64", "install-devcore"],
                machine="aarch64",
            ),
            hermetic_container_integration.IntegrationMode.DIRECT,
        )

    def test_non_build_commands_run_directly(self):
        for command in ["clean", "info", "shutdown", "version", "mod"]:
            with self.subTest(command=command):
                self.assertEqual(
                    self._mode(
                        env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                        args=[command],
                    ),
                    hermetic_container_integration.IntegrationMode.DIRECT,
                )

    def test_missing_docker_still_selects_fail_closed_container_mode(self):
        self.assertEqual(
            self._mode(
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                args=["build", "install-dist-test"],
                docker=False,
            ),
            hermetic_container_integration.IntegrationMode.LINUX_HOST_CONTAINER,
        )

    def test_action_args_enable_dynamic_scheduling_with_remote_execution(self):
        args = hermetic_container_integration._linux_host_container_action_args(
            ["build", "install-dist-test"], env={}
        )
        self.assertEqual(args[0], "build")
        self.assertTrue(any(arg.startswith("--sandbox_base=") for arg in args))
        self.assertIn("--experimental_enable_persistent_container_sandbox", args)
        self.assertTrue(
            any(option.startswith("--experimental_persistent_container_config=") for option in args)
        )
        self.assertTrue(
            any(
                option.startswith("--experimental_persistent_container_worker_dir=")
                for option in args
            )
        )
        self.assertIn("--internal_spawn_scheduler", args)
        self.assertIn("--experimental_dynamic_local_load_factor=0.125", args)
        self.assertFalse(any(option.startswith("--local_resources=") for option in args))
        self.assertFalse(
            any(
                option.startswith(("--local_cpu_resources", "--local_ram_resources"))
                for option in args
            )
        )
        self.assertIn("--experimental_cpp_compile_resource_estimation", args)
        self.assertIn("--strategy=CppCompile=dynamic", args)
        self.assertIn("--dynamic_local_strategy=CppCompile=persistent-container", args)
        self.assertIn("--dynamic_remote_strategy=CppCompile=remote", args)
        self.assertIn("--strategy=Rustc=dynamic", args)
        self.assertIn("--strategy=RustcMetadata=dynamic", args)
        self.assertIn("--dynamic_local_strategy=Rustc=persistent-container", args)
        self.assertIn("--strategy=CargoBuildScriptRun=remote,persistent-container,local", args)
        self.assertIn("--strategy=Genrule=remote,persistent-container,local", args)
        self.assertIn("--strategy=CppLTOIndexing=remote,persistent-container,local", args)
        self.assertIn("--strategy=CcLtoBackendCompile=remote,persistent-container,local", args)
        self.assertIn("--strategy=HistoricRuntime=persistent-container,local", args)
        self.assertIn("--strategy=CppLink=persistent-container,local", args)
        self.assertIn("--strategy=CppArchive=persistent-container,local", args)
        self.assertIn("--strategy=SolibSymlink=persistent-container,local", args)
        self.assertIn("--strategy=ExtractDebugInfo=persistent-container,local", args)
        self.assertIn("--strategy=StripDebugInfo=persistent-container,local", args)
        self.assertIn("--strategy=CcGenerateIntermediateDwp=persistent-container,local", args)
        self.assertIn("--strategy=CcGenerateDwp=persistent-container,local", args)
        self.assertNotIn("--strategy=TestRunner=local", args)
        self.assertNotIn("--strategy=CoverageReport=local", args)
        self.assertIn("install-dist-test", args)

    def test_action_args_dynamic_scheduling_can_be_disabled(self):
        args = hermetic_container_integration._linux_host_container_action_args(
            ["build", "install-dist-test"],
            env={"MONGO_LINUX_DYNAMIC_SCHEDULING": "0"},
        )
        self.assertNotIn("--internal_spawn_scheduler", args)
        self.assertNotIn("--experimental_dynamic_local_load_factor=0.125", args)
        self.assertFalse(any(option.startswith("--local_resources=") for option in args))
        self.assertNotIn("--experimental_cpp_compile_resource_estimation", args)
        self.assertNotIn("--strategy=CppCompile=dynamic", args)
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn("--strategy=Rustc=remote", args)
        self.assertIn("--strategy=CargoBuildScriptRun=remote,persistent-container,local", args)
        self.assertNotIn("--strategy=TestRunner=local", args)
        self.assertNotIn("--strategy=CoverageReport=local", args)
        self.assertIn("--strategy=CppLTOIndexing=remote,persistent-container,local", args)
        self.assertIn("--strategy=CcLtoBackendCompile=remote,persistent-container,local", args)
        self.assertIn("--strategy=HistoricRuntime=persistent-container,local", args)

    def test_action_args_preserve_user_local_resource_settings(self):
        args = hermetic_container_integration._linux_host_container_action_args(
            ["build", "--local_resources=cpu=HOST_CPUS*.5", "install-dist-test"], env={}
        )

        self.assertEqual(
            [option for option in args if option.startswith("--local_resources=")],
            ["--local_resources=cpu=HOST_CPUS*.5"],
        )

    def test_action_args_local_config_sandboxes_containerized_build_tools(self):
        args = hermetic_container_integration._linux_host_container_action_args(
            ["build", "--config=local", "install-dist-test"],
            env={},
        )
        self.assertIn("--strategy=CppCompile=persistent-container,local", args)
        self.assertIn("--strategy=Rustc=persistent-container,local", args)
        self.assertIn("--strategy=RustcMetadata=persistent-container,local", args)
        self.assertIn("--strategy=CargoBuildScriptRun=persistent-container,local", args)
        self.assertIn("--strategy=PyCompile=worker", args)
        self.assertIn("--strategy=CppLTOIndexing=persistent-container,local", args)
        self.assertIn("--strategy=CcLtoBackendCompile=persistent-container,local", args)
        self.assertIn("--strategy=HistoricRuntime=persistent-container,local", args)
        self.assertIn("--strategy=CppLink=persistent-container,local", args)
        self.assertIn("--strategy=CppArchive=persistent-container,local", args)
        for mnemonic in (
            "ExtractCertificateGenerationYear",
            "PyWriteBuildData",
            "WheelInstall",
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=persistent-container,local", args)
        self.assertIn("--strategy=TestRunner=local", args)
        self.assertIn("--strategy=CoverageReport=local", args)
        self.assertIn("--strategy=MongoInstallRule=persistent-container,local", args)
        self.assertNotIn("--internal_spawn_scheduler", args)

    def test_action_args_leave_local_output_actions_remote_for_remote_link(self):
        args = hermetic_container_integration._linux_host_container_action_args(
            ["build", "--config=remote_link", "install-dist-test"], env={}
        )

        for mnemonic in hermetic_container_integration.LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS:
            with self.subTest(mnemonic=mnemonic):
                self.assertNotIn(
                    f"--strategy={mnemonic}=persistent-container,local",
                    args,
                )

    def test_action_args_honor_evergreen_remote_execution_opt_out(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            (repo_root / ".bazelrc.evergreen").write_text(
                "common --remote_executor=\n" "common --modify_execution_info=.*=+no-remote-exec\n",
                encoding="utf-8",
            )

            args = hermetic_container_integration._linux_host_container_action_args(
                ["build", "install-dist-test"],
                env={},
                repo_root=repo_root,
            )

        self.assertIn("--strategy=CppCompile=persistent-container,local", args)
        self.assertIn("--strategy=CargoBuildScriptRun=persistent-container,local", args)
        self.assertIn("--strategy=TestRunner=local", args)
        self.assertNotIn("--strategy=CargoBuildScriptRun=remote,persistent-container,local", args)
        self.assertNotIn("--internal_spawn_scheduler", args)

    def test_action_args_honor_home_and_custom_rc_remote_execution_opt_out(self):
        for rc_option in ("home", "custom"):
            with self.subTest(rc_option=rc_option), tempfile.TemporaryDirectory() as temp_dir:
                temp = pathlib.Path(temp_dir)
                repo_root = temp / "repo"
                repo_root.mkdir()
                home = temp / "home"
                home.mkdir()
                (home / ".bazelrc").write_text(
                    "common:local-alias --remote_executor=grpcs://home.example\n"
                    if rc_option == "custom"
                    else "common:local-alias --remote_executor=\n",
                    encoding="utf-8",
                )
                custom_rc = temp / "custom.bazelrc"
                custom_rc.write_text("common:local-alias --remote_executor=\n", encoding="utf-8")

                args = ["--nosystem_rc", "build", "--config=local-alias"]
                env = {"HOME": str(home)}
                if rc_option == "home":
                    args.insert(1, "--noworkspace_rc")
                else:
                    args.insert(1, "--noworkspace_rc")
                    args.insert(1, f"--bazelrc={custom_rc}")

                args = hermetic_container_integration._linux_host_container_action_args(
                    args,
                    env=env,
                    repo_root=repo_root,
                )

                self.assertIn("--strategy=CppCompile=persistent-container,local", args)
                self.assertNotIn("--internal_spawn_scheduler", args)

    def test_command_line_remote_executor_overrides_evergreen_opt_out(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            (repo_root / ".bazelrc.evergreen").write_text(
                "common --remote_executor=\n",
                encoding="utf-8",
            )

            args = hermetic_container_integration._linux_host_container_action_args(
                [
                    "build",
                    "--remote_executor=grpcs://sodalite.cluster.engflow.com",
                    "install-dist-test",
                ],
                env={},
                repo_root=repo_root,
            )

        self.assertIn("--strategy=CppCompile=dynamic", args)
        self.assertIn("--strategy=CargoBuildScriptRun=remote,persistent-container,local", args)
        self.assertIn("--internal_spawn_scheduler", args)

    def test_container_config_uses_pinned_host_distro_image(self):
        containers = {
            "rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"},
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            with mock.patch.object(
                hermetic_container_integration.shutil,
                "which",
                return_value="/usr/bin/docker",
            ):
                config = hermetic_container_integration._linux_host_container_config(
                    env={
                        "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                        "MONGO_PODMAN_TASK_ID": "task/123",
                    },
                    machine="x86_64",
                    containers=containers,
                    repo_root=repo_root,
                )
        self.assertEqual(config["image"], "quay.io/mongodb/rbe@sha256:abc123")
        self.assertEqual(config["docker_command"], "/usr/bin/docker")
        self.assertEqual(config["podman_task_id"], "task/123")
        self.assertEqual(config["repo_root"], str(repo_root))
        self.assertEqual(config["state_dir"], str(repo_root / ".tmp" / "linux_container_actions"))
        self.assertEqual(
            config["container_layout_version"],
            hermetic_container_integration.LINUX_CONTAINER_ACTIONS_LAYOUT_VERSION,
        )
        self.assertEqual(
            config["home"], str(repo_root / ".tmp" / "linux_container_actions" / "home")
        )
        self.assertTrue(config["container_prefix"].startswith("mongo_linux_action_rhel9_x86_64_"))
        self.assertEqual(config["network"], "host")

    def test_container_config_rejects_mutable_image_override(self):
        containers = {
            "rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"},
        }
        with self.assertRaisesRegex(
            RuntimeError,
            "MONGO_HERMETIC_CONTAINER_IMAGE must use an immutable",
        ):
            hermetic_container_integration._linux_host_container_config(
                env={
                    "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                    "MONGO_HERMETIC_CONTAINER_IMAGE": "docker://example.com/custom:latest",
                },
                machine="x86_64",
                containers=containers,
            )

    def test_container_config_accepts_digest_pinned_image_override(self):
        containers = {
            "rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"},
        }
        image = "docker://example.com/custom@sha256:" + "a" * 64
        with tempfile.TemporaryDirectory() as temp_dir:
            config = hermetic_container_integration._linux_host_container_config(
                env={
                    "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                    "MONGO_HERMETIC_CONTAINER_IMAGE": image,
                },
                machine="x86_64",
                containers=containers,
                repo_root=pathlib.Path(temp_dir),
            )
        self.assertEqual(config["image"], image.removeprefix("docker://"))

    def test_selects_podman_when_docker_is_unavailable(self):
        def which(command):
            return "/usr/bin/podman" if command == "podman" else None

        with (
            mock.patch.object(hermetic_container_integration.shutil, "which", side_effect=which),
            mock.patch.object(
                hermetic_container_integration,
                "_docker_daemon_status",
                return_value=(True, ""),
            ) as daemon_status,
        ):
            command, detail = hermetic_container_integration._select_linux_container_runtime({})

        self.assertEqual(command, "/usr/bin/podman")
        self.assertEqual(detail, "")
        daemon_status.assert_called_once_with("/usr/bin/podman")

    def test_selects_real_podman_when_docker_is_podman_shim(self):
        def which(command):
            return {"docker": "/usr/bin/docker", "podman": "/usr/bin/podman"}.get(command)

        with (
            mock.patch.object(hermetic_container_integration.shutil, "which", side_effect=which),
            mock.patch.object(
                hermetic_container_integration,
                "_is_podman_docker_shim",
                return_value=True,
            ) as is_shim,
            mock.patch.object(
                hermetic_container_integration,
                "_docker_daemon_status",
                return_value=(True, ""),
            ) as daemon_status,
        ):
            command, detail = hermetic_container_integration._select_linux_container_runtime({})

        self.assertEqual(command, "/usr/bin/podman")
        self.assertEqual(detail, "")
        is_shim.assert_called_once_with("/usr/bin/docker")
        daemon_status.assert_called_once_with("/usr/bin/podman")

    def test_detects_podman_docker_shim_from_version_output(self):
        result = subprocess.CompletedProcess(
            args=["/usr/bin/docker", "--version"],
            returncode=0,
            stdout="podman version 5.1.2\n",
            stderr="Emulate Docker CLI using podman. Create /etc/containers/nodocker to quiet msg.\n",
        )
        with mock.patch.object(
            hermetic_container_integration.subprocess, "run", return_value=result
        ) as run:
            is_shim = hermetic_container_integration._is_podman_docker_shim("/usr/bin/docker")

        self.assertTrue(is_shim)
        self.assertEqual(["/usr/bin/docker", "--version"], run.call_args.args[0])

    def test_podman_daemon_status_uses_private_runtime_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            runtime_dir = pathlib.Path(temp_dir) / "runtime"
            with (
                mock.patch.dict(
                    hermetic_container_integration.os.environ,
                    {
                        "TMPDIR": "/data/mci/task/tmp",
                        "XDG_RUNTIME_DIR": "/data/mci/task/runtime",
                    },
                    clear=True,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=runtime_dir,
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

            self.assertTrue(ready)
            self.assertEqual("", detail)
            runtime_env = run.call_args.kwargs["env"]
            self.assertEqual(str(runtime_dir), runtime_env["XDG_RUNTIME_DIR"])
            self.assertEqual(str(runtime_dir), runtime_env["TMPDIR"])
            self.assertEqual(str(runtime_dir), runtime_env["TMP"])
            self.assertEqual(str(runtime_dir), runtime_env["TEMP"])
            self.assertIn("CONTAINERS_STORAGE_CONF", runtime_env)
            storage_config = pathlib.Path(runtime_env["CONTAINERS_STORAGE_CONF"])
            self.assertIn(
                'rootless_storage_path = "',
                storage_config.read_text(encoding="utf-8"),
            )

    def test_podman_daemon_status_migrates_idle_stale_runtime(self):
        stale_error = (
            "invalid internal status, try resetting the pause process with "
            '"/usr/bin/podman system migrate": could not find any running process'
        )
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            runtime_dir = pathlib.Path(temp_dir)
            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=runtime_dir,
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        stale_result,
                        stale_result,
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "ps", "--quiet"],
                            returncode=0,
                            stdout="\n",
                            stderr="",
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "system", "migrate"],
                            returncode=0,
                            stdout="",
                            stderr="",
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "info"],
                            returncode=0,
                            stdout="",
                            stderr="",
                        ),
                    ],
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

            self.assertTrue(ready)
            self.assertEqual("", detail)
            self.assertEqual(
                [
                    ["/usr/bin/podman", "info"],
                    ["/usr/bin/podman", "info"],
                    ["/usr/bin/podman", "ps", "--quiet"],
                    ["/usr/bin/podman", "system", "migrate"],
                    ["/usr/bin/podman", "info"],
                ],
                [call.args[0] for call in run.call_args_list],
            )
            self.assertTrue((runtime_dir / "mongo-podman-recovery.lock").exists())
            for call in run.call_args_list:
                self.assertEqual(
                    str(runtime_dir),
                    call.kwargs["env"]["XDG_RUNTIME_DIR"],
                )

    def test_podman_daemon_status_does_not_migrate_while_containers_run(self):
        stale_error = (
            "invalid internal status, try resetting the pause process with "
            '"/usr/bin/podman system migrate": could not find any running process'
        )
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=pathlib.Path(temp_dir),
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        stale_result,
                        stale_result,
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "ps", "--quiet"],
                            returncode=0,
                            stdout="deadbeef\n",
                            stderr="",
                        ),
                    ],
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

        self.assertFalse(ready)
        self.assertIn("stops the containers currently running", detail)
        self.assertEqual(
            [
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "ps", "--quiet"],
            ],
            [call.args[0] for call in run.call_args_list],
        )

    def test_podman_daemon_status_does_not_migrate_when_container_listing_fails(self):
        stale_error = (
            "invalid internal status, try resetting the pause process with "
            '"/usr/bin/podman system migrate": could not find any running process'
        )
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=pathlib.Path(temp_dir),
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        stale_result,
                        stale_result,
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "ps", "--quiet"],
                            returncode=125,
                            stdout="",
                            stderr=stale_error,
                        ),
                    ],
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

        self.assertFalse(ready)
        self.assertIn("could not determine whether this user has running containers", detail)
        self.assertEqual(
            [
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "ps", "--quiet"],
            ],
            [call.args[0] for call in run.call_args_list],
        )

    def test_podman_daemon_status_can_force_migration_when_container_listing_fails(self):
        stale_error = (
            "invalid internal status, try resetting the pause process with "
            '"/usr/bin/podman system migrate": could not find any running process'
        )
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.dict(
                    hermetic_container_integration.os.environ,
                    {"MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE": "1"},
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=pathlib.Path(temp_dir),
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        stale_result,
                        stale_result,
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "ps", "--quiet"],
                            returncode=125,
                            stdout="",
                            stderr=stale_error,
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "system", "migrate"],
                            returncode=0,
                            stdout="",
                            stderr="",
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "info"],
                            returncode=0,
                            stdout="",
                            stderr="",
                        ),
                    ],
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

        self.assertTrue(ready)
        self.assertEqual("", detail)
        self.assertEqual(
            [
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "ps", "--quiet"],
                ["/usr/bin/podman", "system", "migrate"],
                ["/usr/bin/podman", "info"],
            ],
            [call.args[0] for call in run.call_args_list],
        )

    def test_podman_daemon_status_fails_closed_when_migration_fails(self):
        stale_error = (
            "invalid internal status, try resetting the pause process with "
            '"/usr/bin/podman system migrate": could not find any running process'
        )
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=pathlib.Path(temp_dir),
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        stale_result,
                        stale_result,
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "ps", "--quiet"],
                            returncode=0,
                            stdout="",
                            stderr="",
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "system", "migrate"],
                            returncode=125,
                            stdout="",
                            stderr="migrate: permission denied",
                        ),
                    ],
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

        self.assertFalse(ready)
        self.assertIn("`podman system migrate` failed", detail)
        self.assertIn("permission denied", detail)
        self.assertEqual(
            [
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "ps", "--quiet"],
                ["/usr/bin/podman", "system", "migrate"],
            ],
            [call.args[0] for call in run.call_args_list],
        )

    def test_podman_daemon_status_respects_migration_opt_out(self):
        stale_error = (
            "invalid internal status, try resetting the pause process with "
            '"/usr/bin/podman system migrate": could not find any running process'
        )
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.dict(
                    hermetic_container_integration.os.environ,
                    {"MONGO_BAZEL_PODMAN_AUTO_MIGRATE": "0"},
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=pathlib.Path(temp_dir),
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        stale_result,
                        stale_result,
                    ],
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

        self.assertFalse(ready)
        self.assertIn("MONGO_BAZEL_PODMAN_AUTO_MIGRATE", detail)
        self.assertEqual(
            [
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "info"],
            ],
            [call.args[0] for call in run.call_args_list],
        )

    def test_podman_daemon_status_accepts_runtime_repaired_by_another_process(self):
        stale_error = (
            "invalid internal status, try resetting the pause process with "
            '"/usr/bin/podman system migrate": could not find any running process'
        )
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=pathlib.Path(temp_dir),
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        stale_result,
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "info"],
                            returncode=0,
                        ),
                    ],
                ) as run,
            ):
                ready, detail = hermetic_container_integration._docker_daemon_status(
                    "/usr/bin/podman"
                )

        self.assertTrue(ready)
        self.assertEqual("", detail)
        self.assertEqual(
            [
                ["/usr/bin/podman", "info"],
                ["/usr/bin/podman", "info"],
            ],
            [call.args[0] for call in run.call_args_list],
        )

    def test_podman_image_management_uses_private_runtime_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            runtime_dir = pathlib.Path(temp_dir) / "runtime"
            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=runtime_dir,
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ) as run,
            ):
                self.assertTrue(
                    hermetic_container_integration._ensure_linux_container_image(
                        "/usr/bin/podman", "quay.io/mongodb/rbe@sha256:abc123"
                    )
                )

            runtime_env = run.call_args.kwargs["env"]
            self.assertEqual(str(runtime_dir), runtime_env["XDG_RUNTIME_DIR"])
            self.assertEqual(str(runtime_dir), runtime_env["TMPDIR"])
            self.assertIn("CONTAINERS_STORAGE_CONF", runtime_env)

    def test_image_pull_retries_transient_network_failures(self):
        image = "quay.io/mongodb/rbe@sha256:abc123"
        completed = [
            subprocess.CompletedProcess([], 1),
            subprocess.CompletedProcess([], 1),
            subprocess.CompletedProcess([], 0),
            subprocess.CompletedProcess([], 0),
        ]
        with (
            mock.patch.object(
                hermetic_container_integration.subprocess, "run", side_effect=completed
            ) as run,
            mock.patch.object(hermetic_container_integration.time, "sleep") as sleep,
        ):
            self.assertTrue(
                hermetic_container_integration._ensure_linux_container_image(
                    "/usr/bin/podman", image
                )
            )

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                ["/usr/bin/podman", "image", "inspect", image],
                ["/usr/bin/podman", "pull", image],
                ["/usr/bin/podman", "pull", image],
                ["/usr/bin/podman", "image", "inspect", image],
            ],
        )
        sleep.assert_called_once_with(
            hermetic_container_integration.CONTAINER_NETWORK_RETRY_DELAY_SECONDS
        )

    def test_explicit_container_command_does_not_fall_back(self):
        with mock.patch.object(
            hermetic_container_integration,
            "_docker_daemon_status",
            return_value=(False, "explicit runtime failed"),
        ) as daemon_status:
            command, detail = hermetic_container_integration._select_linux_container_runtime(
                {"HERMETIC_CONTAINER_DOCKER_COMMAND": "/custom/runtime"}
            )

        self.assertIsNone(command)
        self.assertEqual(detail, "explicit runtime failed")
        daemon_status.assert_called_once_with("/custom/runtime")

    def test_dry_run_reports_host_container_plan(self):
        env = {
            "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
            "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
        }
        stdout = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="x86_64"
            ),
            redirect_stdout(stdout),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/bin/true", ["build", "install-dist-test"], env=env
            )
        self.assertEqual(rc, 0)
        payload = json.loads(stdout.getvalue())
        plan = payload["linux_host_container"]
        self.assertIn("--symlink_prefix=bazel-", plan["args"])
        self.assertIn("--experimental_enable_persistent_container_sandbox", plan["args"])
        self.assertIn("--internal_spawn_scheduler", plan["args"])
        self.assertTrue(
            plan["config"]["container_prefix"].startswith("mongo_linux_action_rhel9_x86_64_")
        )
        self.assertTrue(plan["config"]["image"])

    def test_linux_clean_removes_shared_install_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_base = pathlib.Path(temp_dir) / "output-base"
            shared_install_dir = hermetic_container_integration._linux_shared_install_dir(
                output_base
            )
            shared_install_dir.mkdir()
            stale_file = shared_install_dir / "stale-binary"
            stale_file.write_text("stale", encoding="utf-8")
            with mock.patch.object(
                hermetic_container_integration.tempfile,
                "gettempdir",
                return_value=str(pathlib.Path(temp_dir) / "system-temp"),
            ):
                native_shared_install_dir = (
                    hermetic_container_integration._linux_native_shared_install_dir(output_base)
                )
            native_shared_install_dir.mkdir(parents=True)
            native_stale_file = native_shared_install_dir / "stale-binary"
            native_stale_file.write_text("stale", encoding="utf-8")

            with (
                mock.patch.object(
                    hermetic_container_integration.platform, "system", return_value="Linux"
                ),
                mock.patch.object(
                    hermetic_container_integration, "_run_direct", return_value=0
                ) as run_direct,
                mock.patch.object(
                    hermetic_container_integration.tempfile,
                    "gettempdir",
                    return_value=str(pathlib.Path(temp_dir) / "system-temp"),
                ),
            ):
                rc = hermetic_container_integration.run_hermetic_container(
                    "/usr/bin/bazel",
                    [f"--output_base={output_base}", "clean"],
                    env={},
                )

            self.assertEqual(rc, 0)
            run_direct.assert_called_once_with(
                "/usr/bin/bazel", [f"--output_base={output_base}", "clean"]
            )
            self.assertFalse(shared_install_dir.exists())
            self.assertFalse(native_shared_install_dir.exists())

    def test_linux_direct_build_keeps_install_actions_out_of_sandboxes(self):
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration, "_run_direct", return_value=0
            ) as run_direct,
            mock.patch.object(
                hermetic_container_integration, "_publish_linux_shared_install_symlink"
            ) as publish_shared_install,
            mock.patch.object(
                hermetic_container_integration.tempfile,
                "gettempdir",
                return_value="/tmp/host-temp",
            ),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["--output_base=/tmp/output", "build", "install-dist-test"],
                env={"MONGO_LINUX_CONTAINER_ACTIONS": "0"},
            )

        self.assertEqual(rc, 0)
        run_direct.assert_called_once_with(
            "/usr/bin/bazel",
            [
                "--output_base=/tmp/output",
                "build",
                "--strategy=MongoInstallRule=local",
                "install-dist-test",
            ],
        )
        publish_shared_install.assert_called_once_with(
            {"shared_install_dir": "/tmp/host-temp/output-mongo-shared-install"}
        )

    def test_reports_background_container_image_digest(self):
        stderr = StringIO()
        container_config = {
            "image": "quay.io/mongodb/rbe@sha256:abc123",
            "container_name": "mongo_linux_action_rhel9_x86_64_example",
        }
        lock_events = []

        class RecordingLock:
            def __enter__(self):
                lock_events.append("lock_enter")
                return self

            def __exit__(self, *_args):
                lock_events.append("lock_exit")

        def record_lock(_output_base):
            return RecordingLock()

        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="x86_64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_docker_daemon_status",
                return_value=(True, ""),
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_select_linux_container_runtime",
                return_value=("/usr/bin/docker", ""),
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_linux_host_container_config",
                return_value=container_config,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_container_image",
                return_value=True,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_write_linux_container_actions_config_unlocked",
                return_value=(pathlib.Path("/output-base/config.json"), container_config),
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_linux_container_actions_lock",
                side_effect=record_lock,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_action_container",
                return_value=(True, ""),
            ) as ensure_container,
            mock.patch.object(
                hermetic_container_integration,
                "_run_direct",
                side_effect=lambda *_args: lock_events.append("run") or 0,
            ) as run_direct,
            redirect_stderr(stderr),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "install-dist-test"],
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
            )

        self.assertEqual(rc, 0)
        self.assertEqual(lock_events, ["lock_enter", "lock_exit", "run"])
        ensure_container.assert_called_once_with(pathlib.Path("/output-base/config.json"))
        bazel_args = run_direct.call_args.args[1]
        self.assertEqual(bazel_args[0], "--output_base=/output-base")
        self.assertEqual(bazel_args[1], "build")
        self.assertIn("--symlink_prefix=bazel-", bazel_args)
        self.assertIn(
            "Container image sha256:abc123 is running in the "
            "background to execute hermetic build actions.",
            stderr.getvalue(),
        )

    def test_container_start_failure_fails_before_bazel(self):
        container_config = {
            "image": "quay.io/mongodb/rbe@sha256:abc123",
            "container_name": "mongo_linux_action_rhel9_x86_64_example",
        }
        stderr = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="x86_64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_select_linux_container_runtime",
                return_value=("/usr/bin/docker", ""),
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_linux_host_container_config",
                return_value=container_config,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_container_image",
                return_value=True,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_write_linux_container_actions_config_unlocked",
                return_value=(pathlib.Path("/output-base/config.json"), container_config),
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_linux_container_actions_lock",
                return_value=mock.MagicMock(),
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_action_container",
                return_value=(False, "newuidmap: Operation not permitted"),
            ),
            mock.patch.object(hermetic_container_integration, "_run_direct") as run_direct,
            redirect_stderr(stderr),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "install-dist-test"],
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
            )

        self.assertEqual(rc, 1)
        self.assertIn("could not start or reuse build container", stderr.getvalue())
        self.assertIn("newuidmap: Operation not permitted", stderr.getvalue())
        run_direct.assert_not_called()

    def test_missing_docker_fails_closed(self):
        stderr = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="x86_64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_docker_daemon_status",
                return_value=(False, "Docker command not found: docker"),
            ),
            mock.patch.object(hermetic_container_integration, "_run_direct") as run_direct,
            redirect_stderr(stderr),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "install-dist-test"],
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
            )

        self.assertEqual(rc, 1)
        self.assertIn("No usable Linux container runtime", stderr.getvalue())
        self.assertIn("Install Docker Engine (recommended) or Podman", stderr.getvalue())
        self.assertIn("current user", stderr.getvalue())
        self.assertIn("docker info", stderr.getvalue())
        self.assertIn("podman info", stderr.getvalue())
        self.assertIn("refusing to run build tools natively", stderr.getvalue())
        self.assertIn("MONGO_BAZEL_USE_HERMETIC_CONTAINER environment variable", stderr.getvalue())
        self.assertIn("MONGO_BAZEL_USE_HERMETIC_CONTAINER=0", stderr.getvalue())
        run_direct.assert_not_called()

    def test_image_pull_failure_fails_closed(self):
        stderr = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="x86_64"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_docker_daemon_status",
                return_value=(True, ""),
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_container_image",
                return_value=False,
            ),
            mock.patch.object(hermetic_container_integration, "_run_direct") as run_direct,
            redirect_stderr(stderr),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "install-dist-test"],
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
            )

        self.assertEqual(rc, 1)
        self.assertIn("could not pull build container image", stderr.getvalue())
        self.assertIn("refusing to run build tools natively", stderr.getvalue())
        run_direct.assert_not_called()

    def test_explicit_native_opt_out_does_not_check_docker(self):
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration, "_docker_daemon_status"
            ) as docker_status,
            mock.patch.object(
                hermetic_container_integration, "_run_direct", return_value=0
            ) as run_direct,
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "install-dist-test"],
                env={
                    "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                    "MONGO_LINUX_CONTAINER_ACTIONS": "0",
                },
            )

        self.assertEqual(rc, 0)
        docker_status.assert_not_called()
        run_direct.assert_called_once_with(
            "/usr/bin/bazel",
            ["build", "--strategy=MongoInstallRule=local", "install-dist-test"],
        )

    def test_graph_and_fetch_commands_do_not_initialize_container_runtime(self):
        for command in ["aquery", "cquery", "fetch", "query"]:
            with self.subTest(command=command):
                with (
                    mock.patch.object(
                        hermetic_container_integration.platform,
                        "system",
                        return_value="Linux",
                    ),
                    mock.patch.object(
                        hermetic_container_integration.platform,
                        "machine",
                        return_value="x86_64",
                    ),
                    mock.patch.object(
                        hermetic_container_integration,
                        "_select_linux_container_runtime",
                    ) as select_runtime,
                    mock.patch.object(
                        hermetic_container_integration,
                        "_linux_host_container_config",
                    ) as container_config,
                    mock.patch.object(
                        hermetic_container_integration,
                        "_ensure_linux_container_image",
                    ) as ensure_image,
                    mock.patch.object(
                        hermetic_container_integration,
                        "_ensure_linux_action_sandbox_base",
                    ) as ensure_sandbox,
                    mock.patch.object(
                        hermetic_container_integration,
                        "_write_linux_container_actions_config_unlocked",
                    ) as write_config,
                    mock.patch.object(
                        hermetic_container_integration,
                        "_ensure_linux_action_container",
                    ) as ensure_container,
                    mock.patch.object(
                        hermetic_container_integration,
                        "_publish_linux_host_convenience_symlinks",
                    ),
                    mock.patch.object(
                        hermetic_container_integration,
                        "_run_direct",
                        return_value=0,
                    ) as run_direct,
                ):
                    rc = hermetic_container_integration.run_hermetic_container(
                        "/usr/bin/bazel",
                        [command, "//src/mongo:some_target"],
                        env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                    )

                self.assertEqual(rc, 0)
                select_runtime.assert_not_called()
                container_config.assert_not_called()
                ensure_image.assert_not_called()
                ensure_sandbox.assert_not_called()
                write_config.assert_not_called()
                ensure_container.assert_not_called()
                bazel_args = run_direct.call_args.args[1]
                self.assertIn(command, bazel_args)
                self.assertNotIn("--experimental_enable_persistent_container_sandbox", bazel_args)
                self.assertFalse(any(arg.startswith("--sandbox_base=") for arg in bazel_args))
                self.assertFalse(any(arg.startswith("--symlink_prefix=") for arg in bazel_args))


class LinuxHostContainerOutputBaseTest(unittest.TestCase):
    def test_action_sandbox_base_is_a_non_overlapping_output_base_sibling(self):
        output_base = pathlib.Path("/cache/output-base")

        sandbox_base = hermetic_container_integration._linux_action_sandbox_base(output_base)

        self.assertEqual(pathlib.Path("/cache/output-base-mongo-action-sandbox"), sandbox_base)
        self.assertNotEqual(output_base, sandbox_base)
        self.assertNotIn(output_base, sandbox_base.parents)

    def test_ensure_action_sandbox_base_creates_the_sandbox(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_base = pathlib.Path(temp_dir) / "output-base"

            sandbox_base = hermetic_container_integration._ensure_linux_action_sandbox_base(
                output_base
            )

            self.assertTrue(sandbox_base.is_dir())

    def test_shared_install_dir_is_a_non_overlapping_output_base_sibling(self):
        output_base = pathlib.Path("/cache/output-base")

        shared_install_dir = hermetic_container_integration._linux_shared_install_dir(output_base)

        self.assertEqual(
            pathlib.Path("/cache/output-base-mongo-shared-install"), shared_install_dir
        )
        self.assertNotIn(output_base, shared_install_dir.parents)

    def test_linux_native_shared_install_dir_is_outside_output_tree(self):
        output_base = pathlib.Path("/cache/output-base")

        with mock.patch.object(
            hermetic_container_integration.tempfile,
            "gettempdir",
            return_value="/tmp/host-temp",
        ):
            shared_install_dir = hermetic_container_integration._linux_native_shared_install_dir(
                output_base
            )

        self.assertEqual(
            pathlib.Path("/tmp/host-temp/output-base-mongo-shared-install"),
            shared_install_dir,
        )
        self.assertNotIn(output_base, shared_install_dir.parents)

    def test_macos_shared_install_symlink_matches_install_script_fallback(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            repo_root.mkdir()
            output_base = root / "output-base"
            bin_dir = output_base / "execroot" / "_main" / "bazel-out" / "k8-opt" / "bin"
            bin_dir.mkdir(parents=True)
            (repo_root / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)

            with mock.patch.object(
                hermetic_container_integration.tempfile,
                "gettempdir",
                return_value=str(root / "system-temp"),
            ):
                hermetic_container_integration._publish_macos_shared_install_symlink(
                    [f"--output_base={output_base}", "build"], {}, repo_root=repo_root
                )

            install_link = repo_root / "bazel-bin" / "install"
            expected = root / "system-temp" / "output-base-mongo-shared-install" / "k8-opt"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual(expected, install_link.resolve())

    def test_explicit_output_base_startup_option(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    ["--output_base=/some/output/base", "build", "+t"],
                    env={},
                    repo_root=repo_root,
                ),
                pathlib.Path("/some/output/base"),
            )

    def test_computes_default_output_base_from_workspace_md5(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            user_root = temp / "user_root"
            digest = hashlib.md5(str(repo_root).encode(), usedforsecurity=False).hexdigest()
            expected = user_root / digest
            expected.mkdir(parents=True)

            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    [
                        "--nosystem_rc",
                        "--nohome_rc",
                        f"--output_user_root={user_root}",
                        "build",
                    ],
                    env={},
                    repo_root=repo_root,
                ),
                expected,
            )

    def test_new_output_user_root_ignores_stale_convenience_symlink(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            stale_output_base = temp / "outputs" / "abc123"
            bazel_out = stale_output_base / "execroot" / "_main" / "bazel-out"
            bazel_out.mkdir(parents=True)
            (repo_root / "bazel-out").symlink_to(bazel_out)
            user_root = temp / "user_root"
            expected = (
                user_root / hashlib.md5(str(repo_root).encode(), usedforsecurity=False).hexdigest()
            )

            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    [
                        "--nosystem_rc",
                        "--nohome_rc",
                        f"--output_user_root={user_root}",
                        "build",
                    ],
                    env={},
                    repo_root=repo_root,
                ),
                expected,
            )

    def test_test_tmpdir_matches_bazel_default_output_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            test_tmpdir = temp / "bazel-test"
            digest = hashlib.md5(str(repo_root).encode(), usedforsecurity=False).hexdigest()

            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    ["--nosystem_rc", "--nohome_rc", "--noworkspace_rc", "build"],
                    env={"TEST_TMPDIR": str(test_tmpdir)},
                    repo_root=repo_root,
                ),
                test_tmpdir / f"_bazel_{hermetic_container_integration._current_user()}" / digest,
            )

    def test_bazelrc_startup_output_user_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            user_root = temp / "rc_root"
            (repo_root / ".bazelrc").write_text(
                "try-import %workspace%/.bazelrc.local\n", encoding="utf-8"
            )
            (repo_root / ".bazelrc.local").write_text(
                f"startup --output_user_root={user_root}\n", encoding="utf-8"
            )
            digest = hashlib.md5(str(repo_root).encode(), usedforsecurity=False).hexdigest()
            expected = user_root / digest
            expected.mkdir(parents=True)

            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    ["--nosystem_rc", "--nohome_rc", "build"],
                    env={},
                    repo_root=repo_root,
                ),
                expected,
            )

    def test_home_bazelrc_startup_output_base(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            home = temp / "home"
            home.mkdir()
            output_base = temp / "home-output-base"
            (home / ".bazelrc").write_text(
                f"startup --output_base={output_base}\n", encoding="utf-8"
            )

            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    ["--nosystem_rc", "--noworkspace_rc", "build"],
                    env={"HOME": str(home)},
                    repo_root=repo_root,
                ),
                output_base,
            )

    def test_explicit_bazelrc_honors_imported_startup_option(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            custom_rc = temp / "custom.bazelrc"
            imported_rc = repo_root / "imported.bazelrc"
            output_base = temp / "imported-output-base"
            custom_rc.write_text("import %workspace%/imported.bazelrc\n", encoding="utf-8")
            imported_rc.write_text(f"startup --output_base={output_base}\n", encoding="utf-8")

            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    ["--ignore_all_rc_files", f"--bazelrc={custom_rc}", "build"],
                    env={},
                    repo_root=repo_root,
                ),
                # --ignore_all_rc_files intentionally suppresses explicit rc files too.
                pathlib.Path(
                    hermetic_container_integration._default_bazel_user_output_root(
                        {}, system="Linux"
                    )
                )
                / hashlib.md5(str(repo_root).encode(), usedforsecurity=False).hexdigest(),
            )

            self.assertEqual(
                hermetic_container_integration._bazel_output_base(
                    [
                        "--nosystem_rc",
                        "--nohome_rc",
                        "--noworkspace_rc",
                        f"--bazelrc={custom_rc}",
                        "build",
                    ],
                    env={},
                    repo_root=repo_root,
                ),
                output_base,
            )

    def test_replace_output_base_startup_option(self):
        self.assertEqual(
            hermetic_container_integration._replace_bazel_startup_option(
                ["--output_base", "/old", "--batch", "build", "//:target"],
                "--output_base",
                "/new",
            ),
            ["--batch", "--output_base=/new", "build", "//:target"],
        )

    def test_writes_container_actions_config(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            output_base = temp / "output_base"

            with mock.patch.object(
                hermetic_container_integration.tempfile,
                "gettempdir",
                return_value=str(temp / "system-temp"),
            ):
                path, config = hermetic_container_integration._write_linux_container_actions_config(
                    [f"--output_base={output_base}", "build"],
                    env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                    repo_root=repo_root,
                    machine="x86_64",
                )

            self.assertEqual(
                path,
                output_base
                / hermetic_container_integration.LINUX_CONTAINER_ACTIONS_CONFIG_FILENAME,
            )
            on_disk = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(on_disk, config)
            self.assertEqual(on_disk["repo_root"], str(repo_root))
            self.assertEqual(
                on_disk["sandbox_base"],
                str(output_base.with_name(f"{output_base.name}-mongo-action-sandbox")),
            )
            self.assertEqual(
                on_disk["shared_install_dir"],
                str(temp / "system-temp" / "output_base-mongo-shared-install"),
            )
            self.assertRegex(
                on_disk["container_name"],
                r"^mongo_linux_action_rhel9_x86_64_[0-9a-f]{12}_[0-9a-f]{12}$",
            )

    def test_writes_container_actions_config_with_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            output_base = temp / "output_base"

            with mock.patch.object(
                hermetic_container_integration.os,
                "replace",
                wraps=os.replace,
            ) as replace:
                path, _ = hermetic_container_integration._write_linux_container_actions_config(
                    [f"--output_base={output_base}", "build"],
                    env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                    repo_root=repo_root,
                    machine="x86_64",
                )

            replace.assert_called_once()
            temporary_path, destination = replace.call_args.args
            self.assertEqual(destination, path)
            self.assertNotEqual(temporary_path, destination)
            self.assertFalse(pathlib.Path(temporary_path).exists())

    def test_new_output_base_generation_clears_stale_shared_install_files(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            output_base = temp / "output_base"
            with mock.patch.object(
                hermetic_container_integration.tempfile,
                "gettempdir",
                return_value=str(temp / "system-temp"),
            ):
                shared_install_dir = (
                    hermetic_container_integration._linux_native_shared_install_dir(output_base)
                )
                shared_install_dir.mkdir(parents=True)
                stale_file = shared_install_dir / "stale-binary"
                stale_file.write_text("stale", encoding="utf-8")
                legacy_shared_install_dir = (
                    hermetic_container_integration._linux_shared_install_dir(output_base)
                )
                legacy_shared_install_dir.mkdir()
                legacy_stale_file = legacy_shared_install_dir / "legacy-stale-binary"
                legacy_stale_file.write_text("stale", encoding="utf-8")

                hermetic_container_integration._write_linux_container_actions_config(
                    [f"--output_base={output_base}", "build"],
                    env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                    repo_root=repo_root,
                    machine="x86_64",
                )

            self.assertTrue(shared_install_dir.is_dir())
            self.assertFalse(stale_file.exists())
            self.assertFalse(legacy_shared_install_dir.exists())

    def test_output_base_generation_changes_only_when_output_base_is_replaced(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            repo_root = temp / "repo"
            repo_root.mkdir()
            output_base = temp / "output_base"
            args = [f"--output_base={output_base}", "build"]
            kwargs = {
                "env": {"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
                "repo_root": repo_root,
                "machine": "x86_64",
            }

            _, first = hermetic_container_integration._write_linux_container_actions_config(
                args, **kwargs
            )
            _, second = hermetic_container_integration._write_linux_container_actions_config(
                args, **kwargs
            )
            self.assertEqual(first["output_base_generation"], second["output_base_generation"])
            self.assertEqual(first["container_name"], second["container_name"])

            output_base.rename(temp / "stale_output_base")
            _, replacement = hermetic_container_integration._write_linux_container_actions_config(
                args, **kwargs
            )
            self.assertNotEqual(
                first["output_base_generation"], replacement["output_base_generation"]
            )
            self.assertNotEqual(first["container_name"], replacement["container_name"])


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


class HermeticContainerConfigTest(unittest.TestCase):
    def test_windows_path_converts_to_container_path(self):
        self.assertEqual(
            hermetic_container_integration._container_path(
                r"C:\work\mongo\.tmp\hermetic_container", "Windows"
            ),
            "/C/work/mongo/.tmp/hermetic_container",
        )

    def test_builds_config_from_selected_container(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-real"
            bazel_real.write_text("#!/bin/sh\n", encoding="utf-8")
            output_root = repo_root / "cache" / "_bazel_user"
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT": str(output_root),
            }

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Linux",
            )

            self.assertEqual(config.distro, "amazon_linux_2023")
            self.assertEqual(config.docker_image.repository, "quay.io/mongodb")
            self.assertEqual(config.bazel_real, str(bazel_real.resolve()))
            self.assertEqual(config.bazel_command, str(bazel_real.resolve()))
            self.assertTrue(output_root.exists())
            self.assertIn("MONGO_BAZEL_IN_HERMETIC_CONTAINER=1", config.env_vars)
            self.assertTrue(any(str(output_root) in volume for volume in config.volumes))

    def test_full_container_passes_image_and_distro_overrides_to_bazel(self):
        containers = {
            "rhel9": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }
        env = {
            "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
            "MONGO_HERMETIC_CONTAINER_IMAGE": "docker://example.com/custom:latest",
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-real"
            bazel_real.write_text("#!/bin/sh\n", encoding="utf-8")

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Linux",
            )

        self.assertIn("MONGO_HERMETIC_CONTAINER_DISTRO=rhel9", config.env_vars)
        self.assertIn(
            "MONGO_HERMETIC_CONTAINER_IMAGE=docker://example.com/custom:latest",
            config.env_vars,
        )

    def test_linux_full_container_config_accepts_s390x_host_arch(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-real"
            bazel_real.write_text("#!/bin/sh\n", encoding="utf-8")

            with mock.patch.object(
                hermetic_container_integration.platform, "machine", return_value="s390x"
            ):
                config = hermetic_container_integration.build_hermetic_container_config(
                    str(bazel_real),
                    env={"MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023"},
                    repo_root=repo_root,
                    containers=containers,
                    system="Linux",
                )

            self.assertIn("s390x", config.instance_name)

    def test_windows_builds_config_with_container_bazel(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel.exe"
            bazel_real.write_text("", encoding="utf-8")
            output_root = repo_root / "cache" / "_bazel_user"
            llvm_path = repo_root / "llvm"
            llvm_path.mkdir()
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT": str(output_root),
                "MONGO_HERMETIC_CONTAINER_CONTAINER_BAZEL": "/opt/mongodbtoolchain/bazel/bin/bazel",
                "MONGO_WINDOWS_CROSS_LLVM_PATH": str(llvm_path),
            }

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Windows",
            )

            self.assertEqual(config.bazel_command, "/opt/mongodbtoolchain/bazel/bin/bazel")
            self.assertEqual(config.user, "")
            self.assertIn("MONGO_BAZEL_IN_HERMETIC_CONTAINER=1", config.env_vars)
            llvm_container_path = hermetic_container_integration._container_path(
                llvm_path, "Windows"
            )
            self.assertIn(
                f"MONGO_WINDOWS_CROSS_LLVM_PATH={llvm_container_path}",
                config.env_vars,
            )

    def test_macos_builds_config_with_linux_container_bazel(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-darwin"
            bazel_real.write_text("", encoding="utf-8")
            linux_bazel = repo_root / "bazel-linux"
            linux_bazel.write_text("", encoding="utf-8")
            output_root = repo_root / "cache" / "_bazel_user"
            llvm_path = repo_root / "llvm"
            sdk_path = repo_root / "MacOSX15.2.sdk"
            llvm_path.mkdir()
            sdk_path.mkdir()
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT": str(output_root),
                "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL": str(linux_bazel),
                "LLVM_PATH": str(llvm_path),
                "MACOS_SDK_PATH": str(sdk_path),
                "MACOS_MIN_VERSION": "14.0",
                "SDKROOT": "/Applications/Xcode.app/SDKs/MacOSX.sdk",
            }

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Darwin",
            )

            self.assertEqual(config.bazel_command, str(linux_bazel))
            self.assertIn(f"{linux_bazel.parent}:{linux_bazel.parent}:ro", config.volumes)
            self.assertIn(f"{llvm_path}:{llvm_path}:ro", config.volumes)
            self.assertIn(f"{sdk_path}:{sdk_path}:ro", config.volumes)
            self.assertIn(f"LLVM_PATH={llvm_path}", config.env_vars)
            self.assertIn(f"MACOS_SDK_PATH={sdk_path}", config.env_vars)
            self.assertIn("MACOS_MIN_VERSION=14.0", config.env_vars)
            self.assertNotIn("SDKROOT=/Applications/Xcode.app/SDKs/MacOSX.sdk", config.env_vars)
            self.assertEqual(config.docker_image.repository, "mongo-hermetic_container-local")
            self.assertTrue(
                config.docker_image.image_name.startswith("bazel-remote-execution-git:")
            )
            dockerfile = pathlib.Path(config.dockerfile)
            self.assertTrue(dockerfile.is_file())
            dockerfile_text = dockerfile.read_text(encoding="utf-8")
            self.assertIn(
                "FROM quay.io/mongodb/bazel-remote-execution@sha256:abc123",
                dockerfile_text,
            )
            self.assertIn("dnf install -y git", dockerfile_text)
            self.assertIn("python3", dockerfile_text)
            self.assertIn("tar --version", dockerfile_text)

    def test_macos_uses_hermetic_container_local_bazel_output_root_by_default(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-darwin"
            bazel_real.write_text("", encoding="utf-8")
            linux_bazel = repo_root / "bazel-linux"
            linux_bazel.write_text("", encoding="utf-8")
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL": str(linux_bazel),
                "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
            }

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Darwin",
            )

            self.assertEqual(
                config.bazel_user_output_root,
                str(
                    repo_root
                    / ".tmp"
                    / "hermetic_container"
                    / "bazel-output-case-sensitive"
                    / hermetic_container_integration.HERMETIC_CONTAINER_OUTPUT_ROOT_VERSION
                    / f"_bazel_{hermetic_container_integration._current_user()}"
                ),
            )
            self.assertIn(
                f"HOME={repo_root / '.tmp' / 'hermetic_container' / 'home'}",
                config.env_vars,
            )

    def test_macos_case_sensitive_bazel_output_root_can_be_disabled(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            env = {
                hermetic_container_integration.HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT_ENV: "0"
            }

            output_root = hermetic_container_integration._hermetic_container_bazel_user_output_root(
                env,
                repo_root,
                system="Darwin",
            )

            self.assertEqual(
                output_root,
                str(
                    repo_root
                    / ".tmp"
                    / "hermetic_container"
                    / "bazel-output"
                    / hermetic_container_integration.HERMETIC_CONTAINER_OUTPUT_ROOT_VERSION
                    / f"_bazel_{hermetic_container_integration._current_user()}"
                ),
            )

    def test_macos_case_sensitive_output_image_is_created_and_mounted(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)

            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_path_is_case_sensitive",
                    side_effect=[False, True],
                ),
                mock.patch.object(
                    hermetic_container_integration, "_run_host_command"
                ) as run_host_command,
            ):
                output_dir = hermetic_container_integration._macos_case_sensitive_output_dir(
                    repo_root, {}
                )

            image = (
                repo_root
                / ".tmp"
                / "hermetic_container"
                / "bazel-output-case-sensitive.sparsebundle"
            )
            mount_point = repo_root / ".tmp" / "hermetic_container" / "bazel-output-case-sensitive"
            self.assertEqual(output_dir, mount_point)
            self.assertEqual(run_host_command.call_count, 2)
            self.assertEqual(run_host_command.call_args_list[0].args[0][0:2], ["hdiutil", "create"])
            self.assertIn(str(image), run_host_command.call_args_list[0].args[0])
            self.assertEqual(
                run_host_command.call_args_list[1].args[0],
                ["hdiutil", "attach", "-mountpoint", str(mount_point), "-nobrowse", str(image)],
            )

    def test_macos_git_layer_can_be_disabled(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-darwin"
            bazel_real.write_text("", encoding="utf-8")
            linux_bazel = repo_root / "bazel-linux"
            linux_bazel.write_text("", encoding="utf-8")
            output_root = repo_root / "cache" / "_bazel_user"
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT": str(output_root),
                "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL": str(linux_bazel),
                "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
            }

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Darwin",
            )

            self.assertEqual(config.docker_image.repository, "quay.io/mongodb")
            self.assertEqual(
                config.docker_image.image_name,
                "bazel-remote-execution@sha256:abc123",
            )
            self.assertEqual(config.dockerfile, "")

    def test_macos_config_uses_linux_engflow_auth_helper_when_engflow_creds_exist(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            (repo_root / ".bazelrc.engflow_creds").write_text(
                "common --credential_helper=sodalite.cluster.engflow.com=/host/engflow_auth\n",
                encoding="utf-8",
            )
            bazel_real = repo_root / "bazel-darwin"
            bazel_real.write_text("", encoding="utf-8")
            linux_bazel = repo_root / "bazel-linux"
            linux_bazel.write_text("", encoding="utf-8")
            output_root = repo_root / "cache" / "_bazel_user"
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT": str(output_root),
                "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL": str(linux_bazel),
                "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
            }

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Darwin",
            )

            self.assertTrue(
                config.credential_helper.endswith(
                    "/.tmp/hermetic_container/engflow_auth/engflow_auth_linux_arm64/engflow_auth"
                )
            )

    def test_sync_engflow_file_token_exports_and_imports_host_token(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            host_helper = repo_root / "engflow_auth"
            host_helper.write_text("", encoding="utf-8")
            (repo_root / ".bazelrc.engflow_creds").write_text(
                f"common --credential_helper=sodalite.cluster.engflow.com={host_helper}\n",
                encoding="utf-8",
            )

            completed_export = subprocess.CompletedProcess(
                args=[],
                returncode=0,
                stdout=b'{"token": "value"}',
                stderr=b"",
            )
            completed_import = subprocess.CompletedProcess(
                args=[],
                returncode=0,
                stdout=b"",
                stderr=b"",
            )
            with (
                mock.patch.object(hermetic_container_integration.os, "access", return_value=True),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[completed_export, completed_import],
                ) as run,
            ):
                macos_token = (
                    repo_root
                    / "home"
                    / "Library"
                    / "Application Support"
                    / "engflow_auth"
                    / "tokens"
                    / "sodalite.cluster.engflow.com"
                )
                macos_token.parent.mkdir(parents=True)
                macos_token.write_text('{"access_token": "value"}', encoding="utf-8")
                hermetic_container_integration._sync_engflow_file_token(
                    repo_root, {}, repo_root / "home"
                )

            self.assertEqual(run.call_count, 2)
            self.assertEqual(
                run.call_args_list[0].args[0],
                [str(host_helper), "export", "sodalite.cluster.engflow.com"],
            )
            self.assertEqual(
                run.call_args_list[1].args[0],
                [str(host_helper), "import", "--store=file"],
            )
            self.assertEqual(run.call_args_list[1].kwargs["input"], b'{"token": "value"}')
            self.assertEqual(run.call_args_list[1].kwargs["env"]["HOME"], str(repo_root / "home"))
            linux_token = (
                repo_root
                / "home"
                / ".config"
                / "engflow_auth"
                / "tokens"
                / "sodalite.cluster.engflow.com"
            )
            self.assertEqual(linux_token.read_text(encoding="utf-8"), '{"access_token": "value"}')

    def test_mounts_host_ca_bundle_for_grpc_roots(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-real"
            bazel_real.write_text("#!/bin/sh\n", encoding="utf-8")
            ca_bundle = repo_root / "ca-certificates.crt"
            ca_bundle.write_text("certs\n", encoding="utf-8")
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT": str(
                    repo_root / "cache" / "_bazel_user"
                ),
                "MONGO_HERMETIC_CONTAINER_CA_BUNDLE": str(ca_bundle),
            }

            config = hermetic_container_integration.build_hermetic_container_config(
                str(bazel_real),
                env=env,
                repo_root=repo_root,
                containers=containers,
                system="Linux",
            )

            container_ca_bundle = hermetic_container_integration.DEFAULT_CONTAINER_CA_BUNDLE
            grpc_roots_dir = repo_root / ".tmp" / "hermetic_container" / "grpc_roots"
            self.assertIn(f"{ca_bundle}:{container_ca_bundle}:ro", config.volumes)
            self.assertIn(f"{grpc_roots_dir}:/usr/share/grpc:ro", config.volumes)
            self.assertEqual((grpc_roots_dir / "roots.pem").read_text(encoding="utf-8"), "certs\n")
            self.assertIn(f"SSL_CERT_FILE={container_ca_bundle}", config.env_vars)
            self.assertIn(
                f"GRPC_DEFAULT_SSL_ROOTS_FILE_PATH={container_ca_bundle}",
                config.env_vars,
            )

    def test_builds_config_without_host_home(self):
        containers = {
            "amazon_linux_2023": {
                "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
            }
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazel_real = repo_root / "bazel-real"
            bazel_real.write_text("#!/bin/sh\n", encoding="utf-8")
            env = {
                "MONGO_HERMETIC_CONTAINER_DISTRO": "amazon_linux_2023",
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT": str(
                    repo_root / "cache" / "_bazel_user"
                ),
            }

            with mock.patch.object(pathlib.Path, "home", side_effect=RuntimeError("no home")):
                config = hermetic_container_integration.build_hermetic_container_config(
                    str(bazel_real),
                    env=env,
                    repo_root=repo_root,
                    containers=containers,
                    system="Linux",
                )

            self.assertIn(f"HOME={repo_root / '.tmp' / 'hermetic_container'}", config.env_vars)

    def test_config_fingerprint_changes_with_env_vars(self):
        image = hermetic_container_integration.parse_docker_image(
            "docker://quay.io/mongodb/bazel-remote-execution@sha256:abc123"
        )
        base = hermetic_container_integration.HermeticContainerConfig(
            distro="amazon_linux_2023",
            docker_image=image,
            instance_name="test",
            bazel_real="/tmp/bazel",
            bazel_command="/tmp/bazel",
            bazel_user_output_root="/tmp/cache",
            hermetic_container_run_file="/tmp/run",
            user="1:1",
            volumes=["/a:/a"],
            env_vars=["A=1"],
            platform="",
            privileged=False,
        )
        changed = hermetic_container_integration.dataclasses.replace(base, env_vars=["A=2"])

        self.assertNotEqual(
            hermetic_container_integration._config_fingerprint(base),
            hermetic_container_integration._config_fingerprint(changed),
        )

    def test_injects_cert_env_into_bazel_test_args(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            ca_bundle = pathlib.Path(temp_dir) / "ca-certificates.crt"
            ca_bundle.write_text("certs\n", encoding="utf-8")
            env = {"MONGO_HERMETIC_CONTAINER_CA_BUNDLE": str(ca_bundle)}

            args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
                ["test", "//src/..."],
                env,
            )

            container_ca_bundle = hermetic_container_integration.DEFAULT_CONTAINER_CA_BUNDLE
            self.assertEqual(args[0], "test")
            self.assertIn(
                f"--action_env=GRPC_DEFAULT_SSL_ROOTS_FILE_PATH={container_ca_bundle}",
                args,
            )
            self.assertIn(
                f"--test_env=GRPC_DEFAULT_SSL_ROOTS_FILE_PATH={container_ca_bundle}",
                args,
            )
            self.assertEqual(args[-1], "//src/...")

    def test_injects_container_repo_path_into_bazel_args(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            ["build", "//src:mongo"],
            {},
        )

        self.assertIn(
            f"--repo_env=PATH={hermetic_container_integration.HERMETIC_CONTAINER_REPOSITORY_PATH}",
            args,
        )

    def test_injects_container_workspace_status_command_into_bazel_args(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            ["build", "//src:mongo"],
            {},
            workspace_status_command=hermetic_container_integration.HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND,
        )

        self.assertIn(
            f"--workspace_status_command={hermetic_container_integration.HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND}",
            args,
        )

    def test_preserves_user_repo_path_override(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            ["build", "--repo_env=PATH=/custom/bin", "//src:mongo"],
            {},
        )

        self.assertIn("--repo_env=PATH=/custom/bin", args)
        self.assertNotIn(
            f"--repo_env=PATH={hermetic_container_integration.HERMETIC_CONTAINER_REPOSITORY_PATH}",
            args,
        )

    def test_preserves_user_workspace_status_command_override(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            [
                "build",
                "--workspace_status_command=/custom/python workspace_status.py",
                "//src:mongo",
            ],
            {},
            workspace_status_command=hermetic_container_integration.HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND,
        )

        self.assertIn("--workspace_status_command=/custom/python workspace_status.py", args)
        self.assertNotIn(
            f"--workspace_status_command={hermetic_container_integration.HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND}",
            args,
        )

    def test_injects_container_engflow_auth_helper_into_bazel_args(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            ["build", "//src:mongo"],
            {},
            credential_helper="/tmp/engflow_auth",
        )

        self.assertIn(
            "--credential_helper=sodalite.cluster.engflow.com=/tmp/engflow_auth",
            args,
        )

    def test_preserves_user_credential_helper_override(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            [
                "build",
                "--credential_helper=sodalite.cluster.engflow.com=/custom/engflow_auth",
                "//src:mongo",
            ],
            {},
            credential_helper="/tmp/engflow_auth",
        )

        self.assertIn(
            "--credential_helper=sodalite.cluster.engflow.com=/custom/engflow_auth",
            args,
        )
        self.assertNotIn(
            "--credential_helper=sodalite.cluster.engflow.com=/tmp/engflow_auth",
            args,
        )

    def test_windows_cross_config_downloads_top_level_outputs(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            ["build", "--config=windows-cross-x86_64", "install-dist-test"],
            {},
        )

        self.assertEqual(args[0], "build")
        self.assertIn("--remote_download_outputs=toplevel", args)

    def test_macos_cross_config_downloads_top_level_outputs(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            ["build", "--config=macos-cross-arm64", "install-dist-test"],
            {},
        )

        self.assertEqual(args[0], "build")
        self.assertIn("--remote_download_outputs=toplevel", args)

    def test_startup_args_preserved_when_injecting_hermetic_container_env(self):
        args = hermetic_container_integration._bazel_args_with_hermetic_container_env(
            [
                "--output_base=/tmp/custom-output-base",
                "build",
                "--config=macos-cross-arm64",
                "install-dist-test",
            ],
            {},
        )

        self.assertEqual(args[0], "--output_base=/tmp/custom-output-base")
        self.assertEqual(args[1], "build")
        self.assertIn("--remote_download_outputs=toplevel", args[2:])

    def test_prepare_wsl_docker_process_env_sets_host_side_hermetic_container_env(self):
        with mock.patch.dict(hermetic_container_integration.os.environ, {}, clear=True):
            hermetic_container_integration._prepare_hermetic_container_process_env(
                {"MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE": "wsl"}
            )

            self.assertEqual(
                hermetic_container_integration.os.environ["HERMETIC_CONTAINER_VOLUME_SOURCE_MODE"],
                "wsl",
            )
            self.assertEqual(
                hermetic_container_integration.os.environ["DOCKER_HOST"],
                "tcp://127.0.0.1:2375",
            )
            self.assertEqual(
                hermetic_container_integration.os.environ["DOCKER_API_VERSION"], "1.52"
            )

    def test_prepare_wsl_docker_process_env_respects_docker_host_override(self):
        with mock.patch.dict(hermetic_container_integration.os.environ, {}, clear=True):
            hermetic_container_integration._prepare_hermetic_container_process_env(
                {
                    "MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE": "wsl",
                    "DOCKER_HOST": "tcp://127.0.0.1:2376",
                    "DOCKER_API_VERSION": "1.51",
                }
            )

            self.assertEqual(
                hermetic_container_integration.os.environ["DOCKER_HOST"],
                "tcp://127.0.0.1:2376",
            )
            self.assertEqual(
                hermetic_container_integration.os.environ["DOCKER_API_VERSION"], "1.51"
            )


class HermeticContainerRunTest(unittest.TestCase):
    def test_full_container_lock_covers_start_and_exec(self):
        image = hermetic_container_integration.parse_docker_image(
            "quay.io/mongodb/rbe@sha256:abc123"
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            run_file = root / "container.run"
            config = hermetic_container_integration.HermeticContainerConfig(
                distro="amazon_linux_2023",
                docker_image=image,
                instance_name="mongo_hermetic_container_test",
                bazel_real="/tmp/bazel-darwin",
                bazel_command="/tmp/bazel-linux",
                bazel_user_output_root=str(root / "output"),
                hermetic_container_run_file=str(run_file),
                user="1:1",
                volumes=[],
                env_vars=[],
                platform="",
                privileged=False,
            )
            run_file.write_text(
                hermetic_container_integration._config_fingerprint(config) + "\n",
                encoding="utf-8",
            )
            events = []

            docker_instance = mock.Mock()
            docker_instance.hermetic_container_run_file = str(run_file)
            docker_instance.is_running.side_effect = lambda: events.append("is_running") or False
            docker_instance.start.side_effect = lambda: events.append("start") or 0
            docker_instance.send_command.side_effect = lambda _args: events.append("send") or 0

            class RecordingLock:
                def __enter__(self):
                    events.append("lock_enter")
                    return self

                def __exit__(self, *_args):
                    events.append("lock_exit")

            lock_paths = []

            def record_lock(path):
                lock_paths.append(path)
                return RecordingLock()

            container_module = mock.Mock()
            container_module.DockerInstance.return_value = docker_instance

            with (
                mock.patch.object(
                    hermetic_container_integration.platform, "system", return_value="Darwin"
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_prepare_hermetic_container_process_env",
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_docker_daemon_status",
                    return_value=(True, ""),
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "build_hermetic_container_config",
                    return_value=config,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_load_hermetic_container_module",
                    return_value=container_module,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_macos_cross_host_test_requested",
                    return_value=False,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_hermetic_container_lock",
                    side_effect=record_lock,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_publish_hermetic_container_convenience_symlinks",
                ),
            ):
                rc = hermetic_container_integration.run_hermetic_container(
                    "/tmp/bazel-darwin",
                    ["test", "--config=macos-cross-arm64", "//src/mongo:target_test"],
                    env={
                        "MONGO_BAZEL_USE_HERMETIC_CONTAINER": "1",
                        "MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER": "1",
                    },
                )

        self.assertEqual(rc, 0)
        self.assertEqual(events, ["lock_enter", "is_running", "start", "send", "lock_exit"])
        self.assertEqual(lock_paths, [run_file])


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


class HermeticContainerConvenienceSymlinkTest(unittest.TestCase):
    def test_publishes_shared_install_symlink_and_replaces_legacy_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            bin_dir = root / "output" / "execroot" / "_main" / "bazel-out" / "k8-opt" / "bin"
            shared_install_root = root / "shared-install"
            repo_root.mkdir()
            bin_dir.mkdir(parents=True)
            shared_install_root.mkdir()
            (repo_root / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)
            legacy_install = bin_dir / "install"
            legacy_install.mkdir()
            (legacy_install / "old-binary").write_text("old", encoding="utf-8")

            hermetic_container_integration._publish_linux_shared_install_symlink(
                {"shared_install_dir": str(shared_install_root)},
                repo_root=repo_root,
            )

            install_link = repo_root / "bazel-bin" / "install"
            self.assertTrue(install_link.is_symlink())
            self.assertEqual((shared_install_root / "k8-opt").resolve(), install_link.resolve())

    def test_adds_standard_convenience_symlink_prefix(self):
        self.assertEqual(
            hermetic_container_integration._bazel_args_with_hermetic_container_symlink_prefix(
                ["--output_base=/tmp/output", "build", "//src/mongo:mongo"]
            ),
            [
                "--output_base=/tmp/output",
                "build",
                "--symlink_prefix=bazel-",
                "//src/mongo:mongo",
            ],
        )

    def test_preserves_explicit_convenience_symlink_prefix(self):
        args = ["build", "--symlink_prefix=custom/bazel-", "//src/mongo:mongo"]

        self.assertEqual(
            hermetic_container_integration._bazel_args_with_hermetic_container_symlink_prefix(args),
            args,
        )

    def test_temporary_hermetic_container_engflow_bazelrc_does_not_modify_host_contents(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            bazelrc = repo_root / ".bazelrc.engflow_creds"
            host_contents = (
                "common --credential_helper=sodalite.cluster.engflow.com=C:/host/helper.exe\n"
            )
            bazelrc.write_text(host_contents, encoding="utf-8")

            with hermetic_container_integration._temporary_hermetic_container_engflow_bazelrc(
                "/container/engflow_auth",
                repo_root,
            ) as dedicated_bazelrc:
                self.assertIsNotNone(dedicated_bazelrc)
                dedicated_path = pathlib.Path(dedicated_bazelrc)
                self.assertNotEqual(dedicated_path, bazelrc)
                self.assertEqual(
                    dedicated_path.read_text(encoding="utf-8"),
                    (
                        "common --credential_helper="
                        "sodalite.cluster.engflow.com=/container/engflow_auth\n"
                    ),
                )
                self.assertEqual(bazelrc.read_text(encoding="utf-8"), host_contents)

            self.assertFalse(dedicated_path.exists())
            self.assertEqual(bazelrc.read_text(encoding="utf-8"), host_contents)

    def test_restores_hermetic_container_convenience_symlinks_from_marker(self):
        class FakeDockerInstance:
            workspace_hex_digest = "container-hash"
            bazel_output_base_digest = "output-base-hash"

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            repo_root = root / "repo"
            repo_root.mkdir()
            output_root = root / "out"
            execroot = output_root / "output-base-hash" / "execroot" / "_main"
            bazel_out = execroot / "bazel-out"
            bin_dir = bazel_out / "aarch64-fastbuild" / "bin"
            testlogs_dir = bazel_out / "aarch64-fastbuild" / "testlogs"
            bin_dir.mkdir(parents=True)
            testlogs_dir.mkdir(parents=True)
            host_bin = root / "host-bin"
            host_bin.mkdir()
            convenience_dir = repo_root
            (convenience_dir / "bazel-bin").symlink_to(bin_dir, target_is_directory=True)
            (convenience_dir / "bazel-out").symlink_to(bazel_out, target_is_directory=True)
            (convenience_dir / "bazel-testlogs").symlink_to(testlogs_dir, target_is_directory=True)

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
                hermetic_container_run_file=str(root / "container.run"),
                user="1:1",
                volumes=[],
                env_vars=[],
                platform="",
                privileged=False,
            )
            marker = root / "symlinks.json"

            hermetic_container_integration._publish_hermetic_container_convenience_symlinks(
                config,
                FakeDockerInstance(),
                {
                    hermetic_container_integration.HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV: str(
                        marker
                    )
                },
                repo_root=repo_root,
            )
            (repo_root / "bazel-bin").unlink()
            (repo_root / "bazel-bin").symlink_to(host_bin, target_is_directory=True)

            hermetic_container_integration.restore_hermetic_container_convenience_symlinks_from_env(
                {
                    hermetic_container_integration.HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV: str(
                        marker
                    )
                }
            )

            self.assertEqual((repo_root / "bazel-bin").resolve(), bin_dir.resolve())
            self.assertEqual((repo_root / "bazel-out").resolve(), bazel_out.resolve())
            self.assertEqual((repo_root / "bazel-testlogs").resolve(), testlogs_dir.resolve())
            self.assertEqual((repo_root / "bazel-repo").resolve(), execroot.resolve())


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


class WindowsCrossSysrootTest(unittest.TestCase):
    def test_run_hermetic_container_dry_run_plans_windows_cross_host_run(self):
        image = hermetic_container_integration.parse_docker_image(
            "quay.io/mongodb/rbe@sha256:abc123"
        )
        config = hermetic_container_integration.HermeticContainerConfig(
            distro="amazon_linux_2023",
            docker_image=image,
            instance_name="test",
            bazel_real="C:/bazel.exe",
            bazel_command="/tmp/bazel-linux",
            bazel_user_output_root="C:/tmp/output",
            hermetic_container_run_file="C:/tmp/run",
            user="",
            volumes=[],
            env_vars=[],
            platform="",
            privileged=False,
        )

        stdout = StringIO()
        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Windows"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "build_hermetic_container_config",
                return_value=config,
            ),
            redirect_stdout(stdout),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "C:/bazel.exe",
                ["run", "//src/mongo/stdx:stdx_test", "--", "--fileNameFilter", "stdx"],
                env={
                    "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
                    "MONGO_WINDOWS_CROSS_DEFAULT_CONFIG": "1",
                    "MONGO_WINDOWS_CROSS_SYSROOT_URL": "https://example.invalid/sysroot.tar.xz",
                    "MONGO_WINDOWS_CROSS_SYSROOT_SHA256": "abc123",
                },
            )

        self.assertEqual(rc, 0)
        dry_run = json.loads(stdout.getvalue().splitlines()[-1])
        self.assertNotIn("hermetic_container_config", dry_run)
        self.assertIn("windows_cross_host_wrapper_run", dry_run)
        self.assertEqual(dry_run["windows_cross_host_wrapper_run"]["build_args"][0], "build")
        self.assertIn(
            f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}",
            dry_run["windows_cross_host_wrapper_run"]["build_args"],
        )
        self.assertIn(
            "//src/mongo/stdx:stdx_test",
            dry_run["windows_cross_host_wrapper_run"]["build_args"],
        )
        self.assertIn(
            "--//bazel/config:windows_cross_local_container_actions=True",
            dry_run["windows_cross_host_wrapper_run"]["build_args"],
        )
        self.assertIn(
            "--remote_executor=grpcs://sodalite.cluster.engflow.com",
            dry_run["windows_cross_host_wrapper_run"]["build_args"],
        )
        self.assertIn(
            "--strategy=CppCompile=remote",
            dry_run["windows_cross_host_wrapper_run"]["build_args"],
        )
        self.assertIn(
            "--strategy=IdlcGenerator=remote",
            dry_run["windows_cross_host_wrapper_run"]["build_args"],
        )
        self.assertEqual(
            dry_run["windows_cross_host_wrapper_run"]["target"], "//src/mongo/stdx:stdx_test"
        )
        self.assertEqual(
            dry_run["windows_cross_host_wrapper_run"]["run_args"], ["--fileNameFilter", "stdx"]
        )

    def test_plans_windows_cross_host_wrapper_action_args(self):
        args = hermetic_container_integration._windows_cross_host_wrapper_action_args(
            [
                "build",
                "--config=windows-cross-x86_64",
                "install-dist-test",
            ],
            {
                "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
                "MONGO_WINDOWS_CROSS_SYSROOT_PATH": "C:/sysroot",
                "MONGO_WINDOWS_CROSS_LLVM_PATH": "C:/llvm",
                "PATH": "C:/tools;C:/python",
            },
            containers={
                "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
            },
        )

        self.assertEqual(args[0], "build")
        self.assertIn("--//bazel/config:windows_cross_local_container_actions=True", args)
        self.assertIn("--//bazel/config:idl_use_linux_python=True", args)
        self.assertIn("--//bazel/config:disable_warnings_as_errors=True", args)
        self.assertIn("--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=1", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=x86_64", args)
        self.assertIn("--repo_env=MONGO_WINDOWS_CROSS_SYSROOT_PATH=C:/sysroot", args)
        self.assertIn("--repo_env=MONGO_WINDOWS_CROSS_LLVM_PATH=C:/llvm", args)
        self.assertIn("--//bazel/config:windows_cross_host_path=C:/tools;C:/python", args)
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", args)
        self.assertIn(
            "--remote_default_exec_properties=container-image=docker://quay.io/mongodb/rbe@sha256:abc123",
            args,
        )
        self.assertIn("--remote_default_exec_properties=dockerNetwork=standard", args)
        self.assertIn("--remote_default_exec_properties=Pool=x86_64", args)
        self.assertIn("--//bazel/config:remote_link=True", args)
        self.assertIn("--spawn_strategy=local", args)
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn("--strategy=CppLink=remote", args)
        self.assertIn("--strategy=CppArchive=remote", args)
        self.assertIn("--strategy=ConfigHeaderGen=remote", args)
        self.assertIn("--strategy=IdlcGenerator=remote", args)
        self.assertIn("--strategy=WindowsRC=remote", args)
        self.assertIn("--features=-thin_archive", args)
        self.assertIn("--test_strategy=standalone", args)
        self.assertIn("--strategy=TestRunner=standalone", args)
        self.assertIn(
            "--action_env=MONGO_WINDOWS_CROSS_ACTION_IMAGE=quay.io/mongodb/rbe@sha256:abc123",
            args,
        )
        self.assertFalse(
            any(
                argument.startswith("--action_env=")
                and any(
                    variable in argument
                    for variable in (
                        "MONGO_WINDOWS_CROSS_ACTION_DOCKER_COMMAND",
                        "MONGO_WINDOWS_CROSS_ACTION_REPO_ROOT",
                        "MONGO_WINDOWS_CROSS_ACTION_HOME",
                        "MONGO_WINDOWS_CROSS_ACTION_WRAPPER_SCRIPT",
                        "MONGO_WINDOWS_CROSS_ACTION_PYTHON",
                        "MONGO_WINDOWS_CROSS_LLVM_PATH",
                        "MONGO_WINDOWS_CROSS_SYSROOT_PATH",
                        "DOCKER_HOST",
                        "DOCKER_CONTEXT",
                        "DOCKER_CONFIG",
                    )
                )
                for argument in args
            )
        )

    def test_plans_linux_s390x_cross_rbe_host_args(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "test",
                "--config=linux-s390x-cross-rbe",
                "//src/mongo/stdx:stdx_test",
            ],
            {},
            containers={"rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}},
        )

        self.assertEqual(args[0], "test")
        self.assertIn("--config=linux-s390x-cross-rbe", args)
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", args)
        self.assertIn(
            "--remote_default_exec_properties=container-image=docker://quay.io/mongodb/rbe@sha256:abc123",
            args,
        )
        self.assertIn("--remote_default_exec_properties=dockerNetwork=standard", args)
        self.assertIn("--remote_default_exec_properties=Pool=x86_64", args)
        self.assertIn("--extra_execution_platforms=//bazel/platforms:rhel9_amd64", args)
        self.assertIn("--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=1", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=x86_64", args)
        self.assertIn("--//bazel/config:idl_use_linux_python=True", args)
        self.assertIn("--//bazel/config:remote_link=True", args)
        self.assertIn("--spawn_strategy=remote,local", args)
        self.assertNotIn("--spawn_strategy=local", args)
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn("--strategy=CppLink=remote", args)
        self.assertIn("--strategy=CppArchive=remote", args)
        self.assertIn("--strategy=IdlcGenerator=remote", args)
        self.assertIn("--test_strategy=standalone", args)
        self.assertIn("--strategy=TestRunner=standalone", args)

    def test_plans_linux_ppc64le_cross_rbe_arm_host_args(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "build",
                "--config=linux-ppc64le-rhel8-cross-rbe-arm64",
                "install-dist-test",
            ],
            {},
            containers={"rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}},
        )

        self.assertIn("--extra_execution_platforms=//bazel/platforms:rhel9_arm64", args)
        self.assertIn("--remote_default_exec_properties=Pool=default", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=aarch64", args)

    def test_run_hermetic_container_uses_linux_cross_rbe_host_bazel(self):
        containers = {"rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}}

        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
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
                "/usr/bin/bazel",
                ["build", "--config=linux-s390x-cross-rbe", "install-dist-test"],
                env={},
            )

        self.assertEqual(rc, 0)
        docker_status.assert_not_called()
        run_direct.assert_called_once()
        self.assertEqual(run_direct.call_args.args[0], "/usr/bin/bazel")
        host_args = run_direct.call_args.args[1]
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", host_args)
        self.assertIn("--strategy=CppCompile=remote", host_args)
        self.assertIn("--strategy=CppLink=remote", host_args)
        self.assertIn("--extra_execution_platforms=//bazel/platforms:rhel9_amd64", host_args)

    def test_run_hermetic_container_uses_windows_cross_host_wrapper_build(self):
        containers = {
            "amazon_linux_2023": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
        }

        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Windows"
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
                "C:/bazel.exe",
                ["build", "install-dist-test"],
                env={
                    "MONGO_HERMETIC_CONTAINER_GIT_LAYER": "0",
                    "MONGO_WINDOWS_CROSS_DEFAULT_CONFIG": "1",
                    "MONGO_WINDOWS_CROSS_SYSROOT_URL": "https://example.invalid/sysroot.tar.xz",
                    "MONGO_WINDOWS_CROSS_SYSROOT_SHA256": "abc123",
                },
            )

        self.assertEqual(rc, 0)
        docker_status.assert_not_called()
        run_direct.assert_called_once()
        self.assertEqual(run_direct.call_args.args[0], "C:/bazel.exe")
        host_args = run_direct.call_args.args[1]
        self.assertEqual(host_args[0], "build")
        self.assertIn(f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}", host_args)
        self.assertIn("install-dist-test", host_args)
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", host_args)
        self.assertIn("--strategy=CppCompile=remote", host_args)
        self.assertIn("--strategy=CppLink=remote", host_args)
        self.assertIn("--strategy=IdlcGenerator=remote", host_args)
        self.assertIn("--strategy=WindowsRC=remote", host_args)

    def test_reads_pinned_windows_repo_envs_from_bazelrc_and_args(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            repo_root.joinpath(".bazelrc").write_text(
                'common:windows --repo_env=BAZEL_VC="C:/VS/VC"\n'
                "common:windows --repo_env=BAZEL_VC_FULL_VERSION=14.44.35207\n"
                "common:windows --repo_env=BAZEL_WINSDK_FULL_VERSION=10.0.20348.0\n",
                encoding="utf-8",
            )

            values = hermetic_container_integration._windows_cross_repo_env_values(
                [
                    "build",
                    "--config=windows-cross-x86_64",
                    "--repo_env=BAZEL_WINSDK_FULL_VERSION=10.0.26100.0",
                ],
                {},
                repo_root,
            )

            self.assertEqual(values["BAZEL_VC"], "C:/VS/VC")
            self.assertEqual(values["BAZEL_VC_FULL_VERSION"], "14.44.35207")
            self.assertEqual(values["BAZEL_WINSDK_FULL_VERSION"], "10.0.26100.0")

    def test_prepares_generated_sysroot_from_pinned_host_layout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            vc_version = "14.44.35207"
            sdk_version = "10.0.20348.0"
            vc_path = repo_root / "VS" / "VC"
            sdk_root = repo_root / "Windows Kits" / "10"
            env = {
                "BAZEL_VC": str(vc_path),
                "BAZEL_VC_FULL_VERSION": vc_version,
                "BAZEL_WINSDK_FULL_VERSION": sdk_version,
                "MONGO_WINDOWS_CROSS_WINSDK_ROOT": str(sdk_root),
            }
            spec = hermetic_container_integration._windows_cross_sysroot_spec(env)
            for relative, source in spec.sources.items():
                source.mkdir(parents=True)
                source.joinpath("sentinel.txt").write_text(relative, encoding="utf-8")

            prepared = hermetic_container_integration._prepare_windows_cross_env(
                ["build", "--config=windows-cross-x86_64", "install-dist-test"],
                env,
                repo_root,
                "Windows",
            )

            sysroot_path = pathlib.Path(prepared["MONGO_WINDOWS_CROSS_SYSROOT_PATH"])
            self.assertIn(f"msvc-{vc_version}", sysroot_path.name)
            self.assertIn(f"winsdk-{sdk_version}", sysroot_path.name)
            self.assertEqual(prepared["BAZEL_VC_FULL_VERSION"], vc_version)
            self.assertEqual(prepared["BAZEL_WINSDK_FULL_VERSION"], sdk_version)
            self.assertEqual(prepared["MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE"], "wsl")
            for relative in spec.sources:
                self.assertTrue((sysroot_path / relative / "sentinel.txt").is_file())

    def test_windows_cross_env_preserves_explicit_docker_host_mode(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            sysroot_path = repo_root / "existing-sysroot"
            sysroot_path.mkdir()
            env = {
                "MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE": "desktop",
                "MONGO_WINDOWS_CROSS_SYSROOT_PATH": str(sysroot_path),
            }

            prepared = hermetic_container_integration._prepare_windows_cross_env(
                ["build", f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}"],
                env,
                repo_root,
                "Windows",
            )

            self.assertEqual(prepared["MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE"], "desktop")

    def test_explicit_sysroot_path_skips_host_generation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            sysroot_path = repo_root / "existing-sysroot"
            sysroot_path.mkdir()
            env = {"MONGO_WINDOWS_CROSS_SYSROOT_PATH": str(sysroot_path)}

            prepared = hermetic_container_integration._prepare_windows_cross_env(
                ["build", "--config=windows-cross-x86_64"],
                env,
                repo_root,
                "Windows",
            )

            self.assertEqual(prepared["MONGO_WINDOWS_CROSS_SYSROOT_PATH"], str(sysroot_path))

    def test_sysroot_archive_skips_host_generation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            env = {
                "MONGO_WINDOWS_CROSS_SYSROOT_URL": "https://example.invalid/sysroot.tar.xz",
                "MONGO_WINDOWS_CROSS_SYSROOT_SHA256": "abc123",
            }

            prepared = hermetic_container_integration._prepare_windows_cross_env(
                ["build", f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}"],
                env,
                repo_root,
                "Windows",
            )

            self.assertNotIn("MONGO_WINDOWS_CROSS_SYSROOT_PATH", prepared)

    def test_sysroot_archive_repo_env_skips_host_generation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)

            prepared = hermetic_container_integration._prepare_windows_cross_env(
                [
                    "build",
                    f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}",
                    "--repo_env=MONGO_WINDOWS_CROSS_SYSROOT_URL=https://example.invalid/sysroot.tar.xz",
                    "--repo_env=MONGO_WINDOWS_CROSS_SYSROOT_SHA256=abc123",
                ],
                {},
                repo_root,
                "Windows",
            )

            self.assertNotIn("MONGO_WINDOWS_CROSS_SYSROOT_PATH", prepared)

    def test_sysroot_archive_bazelrc_repo_env_skips_host_generation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            repo_root.joinpath(".bazelrc").write_text(
                "common:windows-cross-x86_64 "
                "--repo_env=MONGO_WINDOWS_CROSS_SYSROOT_URL=https://example.invalid/sysroot.tar.xz\n"
                "common:windows-cross-x86_64 "
                "--repo_env=MONGO_WINDOWS_CROSS_SYSROOT_SHA256=abc123\n",
                encoding="utf-8",
            )

            prepared = hermetic_container_integration._prepare_windows_cross_env(
                ["build", f"--config={hermetic_container_integration.WINDOWS_CROSS_CONFIG}"],
                {},
                repo_root,
                "Windows",
            )

            self.assertNotIn("MONGO_WINDOWS_CROSS_SYSROOT_PATH", prepared)


if __name__ == "__main__":
    unittest.main()
