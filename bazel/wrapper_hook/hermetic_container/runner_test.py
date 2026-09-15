"""Unit tests for bazel.wrapper_hook.hermetic_container.runner."""

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


if __name__ == "__main__":
    unittest.main()
