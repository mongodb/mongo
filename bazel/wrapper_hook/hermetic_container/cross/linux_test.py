"""Unit tests for bazel.wrapper_hook.hermetic_container.cross.linux."""

from __future__ import annotations

import errno
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
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

    def test_mandatory_command_options_follow_user_options_and_precede_run_args(self):
        args = hermetic_container_integration._append_bazel_command_options_last(
            [
                "test",
                "//src/mongo:target",
                "--config=remote_test",
                "--",
                "user-argument",
            ],
            ["--strategy=TestRunner=standalone"],
        )

        self.assertEqual(
            args,
            [
                "test",
                "//src/mongo:target",
                "--config=remote_test",
                "--strategy=TestRunner=standalone",
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

    def test_missing_docker_still_selects_container_mode_for_runtime_fallback(self):
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
        self.assertIn("--strategy=Javac=local", args)
        self.assertIn("--strategy=Turbine=local", args)
        self.assertIn("--strategy=JavaToolchainCompileBootClasspath=local", args)
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

    def test_container_config_preserves_registry_auth_file_for_task_podman(self):
        containers = {
            "rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"},
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            auth_file = root / "runtime" / "containers" / "auth.json"
            auth_file.parent.mkdir(parents=True)
            auth_file.write_text("{}\n", encoding="utf-8")
            config = hermetic_container_integration._linux_host_container_config(
                env={
                    "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                    "MONGO_PODMAN_TASK_ID": "task/123",
                    "XDG_RUNTIME_DIR": str(root / "runtime"),
                },
                machine="x86_64",
                containers=containers,
                repo_root=root,
            )

        self.assertEqual(config["podman_auth_file"], str(auth_file))

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

    def test_required_podman_wins_over_healthy_docker(self):
        def which(command):
            return {"docker": "/usr/bin/docker", "podman": "/usr/bin/podman"}.get(command)

        with (
            mock.patch.object(hermetic_container_integration.shutil, "which", side_effect=which),
            mock.patch.object(
                hermetic_container_integration,
                "_is_podman_docker_shim",
                side_effect=AssertionError("Docker must not be inspected when Podman is required"),
            ) as is_shim,
            mock.patch.object(
                hermetic_container_integration,
                "_docker_daemon_status",
                return_value=(True, ""),
            ) as daemon_status,
        ):
            command, detail = hermetic_container_integration._select_linux_container_runtime(
                {"MONGO_PODMAN_REQUIRED": "true"}
            )

        self.assertEqual(command, "/usr/bin/podman")
        self.assertEqual(detail, "")
        is_shim.assert_not_called()
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
                        "MONGO_PODMAN_TASK_ID": "task-123",
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
            containers_config = pathlib.Path(runtime_env["CONTAINERS_CONF"])
            self.assertEqual(containers_config, runtime_dir / "containers.conf")
            self.assertEqual(
                containers_config.read_text(encoding="utf-8"),
                '[engine]\ncgroup_manager = "cgroupfs"\n',
            )
            storage_config = pathlib.Path(runtime_env["CONTAINERS_STORAGE_CONF"])
            self.assertIn(
                'rootless_storage_path = "',
                storage_config.read_text(encoding="utf-8"),
            )

    def test_podman_daemon_status_skips_containers_config_without_task_id(self):
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
            # Without a task ID the runtime is shared, so no containers.conf is
            # generated and Podman keeps its own defaults.
            self.assertNotIn("CONTAINERS_CONF", runtime_env)
            self.assertFalse((runtime_dir / "containers.conf").exists())

    def test_ensure_owned_podman_directory_accepts_owned_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = pathlib.Path(temp_dir) / "task-root"
            path.mkdir(mode=0o755)
            hermetic_container_integration._ensure_owned_podman_directory(path, os.getuid())
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)

    def test_ensure_owned_podman_directory_rejects_foreign_owner(self):
        if os.getuid() == 0:
            self.skipTest("root owns every directory, foreign-uid check cannot fail")
        with tempfile.TemporaryDirectory() as temp_dir:
            path = pathlib.Path(temp_dir) / "task-root"
            path.mkdir(mode=0o700)
            with self.assertRaises(OSError):
                hermetic_container_integration._ensure_owned_podman_directory(path, os.getuid() + 1)

    def test_ensure_owned_podman_directory_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            target = pathlib.Path(temp_dir) / "elsewhere"
            target.mkdir()
            link = pathlib.Path(temp_dir) / "task-root"
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaises(OSError):
                hermetic_container_integration._ensure_owned_podman_directory(link, os.getuid())

    def test_ensure_owned_podman_directory_rejects_regular_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = pathlib.Path(temp_dir) / "task-root"
            path.write_text("not a directory\n", encoding="utf-8")
            with self.assertRaises(OSError):
                hermetic_container_integration._ensure_owned_podman_directory(path, os.getuid())

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
                mock.patch.dict(
                    hermetic_container_integration.os.environ,
                    {
                        "MONGO_PODMAN_TASK_ID": "",
                        "MONGO_BAZEL_PODMAN_AUTO_MIGRATE": "1",
                        "MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE": "0",
                    },
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

    def test_podman_daemon_status_recovers_task_scoped_runtime_when_listing_is_stale(self):
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
            task_root = (
                pathlib.Path(temp_dir) / "mongo-linux-podman-task-mongodb_mongo_master_task-0"
            )
            runtime_dir = task_root / "mongo-linux-podman-runtime-1000"
            runtime_dir.mkdir(parents=True)
            with (
                mock.patch.dict(
                    hermetic_container_integration.os.environ,
                    {
                        "MONGO_PODMAN_TASK_ID": "mongodb_mongo_master_task-0",
                        "MONGO_BAZEL_PODMAN_AUTO_MIGRATE": "1",
                        "MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE": "0",
                    },
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_task_root",
                    return_value=task_root,
                ),
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

    def test_podman_daemon_status_does_not_migrate_task_scoped_runtime_on_unrelated_failure(self):
        # A task-scoped runtime is isolated, but an unrelated `podman ps` failure is
        # not evidence that the runtime is stale: containers may still be running and
        # reachable, so migration must stay refused.
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
                    {
                        "MONGO_PODMAN_TASK_ID": "mongodb_mongo_master_task-0",
                        "MONGO_BAZEL_PODMAN_AUTO_MIGRATE": "1",
                        "MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE": "0",
                    },
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
                            stderr="error opening database: permission denied",
                        ),
                        # Only reached if the guard wrongly authorizes migration.
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

        self.assertFalse(ready)
        self.assertIn("could not determine whether this user has running containers", detail)
        self.assertIn("permission denied", detail)
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

    def test_task_scoped_podman_panic_is_recovered_with_reset(self):
        stale_error = "invalid internal status"
        stale_result = subprocess.CompletedProcess(
            args=["/usr/bin/podman", "info"],
            returncode=125,
            stdout="",
            stderr=stale_error,
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"
            runtime_dir = task_root / "mongo-linux-podman-runtime-1000"
            runtime_dir.mkdir(parents=True)
            with (
                mock.patch.dict(
                    hermetic_container_integration.os.environ,
                    {"MONGO_PODMAN_TASK_ID": "task/123"},
                    clear=True,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_task_root",
                    return_value=task_root,
                ),
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
                            returncode=125,
                            stdout="",
                            stderr=stale_error,
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "system", "migrate"],
                            returncode=-6,
                            stdout="",
                            stderr="",
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "unshare", "umount"],
                            returncode=0,
                            stdout="",
                            stderr="",
                        ),
                        subprocess.CompletedProcess(
                            args=["/usr/bin/podman", "system", "reset"],
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
                [
                    "/usr/bin/podman",
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
                ["/usr/bin/podman", "system", "reset", "--force"],
                ["/usr/bin/podman", "info"],
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

    def test_image_pull_retries_without_rejected_podman_credentials(self):
        image = "quay.io/mongodb/rbe@sha256:abc123"
        rejected = subprocess.CompletedProcess(
            ["/usr/bin/podman", "pull", image],
            125,
            stderr="invalid username/password: unauthorized",
        )
        pulled = subprocess.CompletedProcess(["/usr/bin/podman", "pull", image], 0)

        with tempfile.TemporaryDirectory() as temp_dir:
            runtime_dir = pathlib.Path(temp_dir) / "runtime"
            auth_file = pathlib.Path(temp_dir) / "auth.json"
            auth_file.write_text('{"auths": {"quay.io": {"auth": "expired"}}}')
            with (
                mock.patch.dict(
                    os.environ,
                    {
                        "MONGO_PODMAN_TASK_ID": "task-123",
                        "REGISTRY_AUTH_FILE": str(auth_file),
                    },
                    clear=False,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=runtime_dir,
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        subprocess.CompletedProcess([], 1),
                        subprocess.CompletedProcess([], 0),
                    ],
                ) as run,
                mock.patch.object(
                    hermetic_container_integration,
                    "_run_container_network_command",
                    side_effect=[rejected, pulled],
                ) as network_command,
            ):
                self.assertTrue(
                    hermetic_container_integration._ensure_linux_container_image(
                        "/usr/bin/podman", image
                    )
                )

        self.assertEqual(2, network_command.call_count)
        first_env = network_command.call_args_list[0].kwargs["env"]
        second_env = network_command.call_args_list[1].kwargs["env"]
        self.assertEqual(str(auth_file), first_env["REGISTRY_AUTH_FILE"])
        self.assertNotIn("REGISTRY_AUTH_FILE", second_env)
        self.assertNotEqual(first_env["HOME"], second_env["HOME"])
        self.assertEqual(2, run.call_count)

    def test_image_pull_retries_when_shim_writes_auth_failure_to_stdout(self):
        image = "quay.io/mongodb/rbe@sha256:abc123"
        rejected = subprocess.CompletedProcess(
            ["/usr/bin/podman", "pull", image],
            125,
            stdout="invalid username/password: unauthorized",
        )
        pulled = subprocess.CompletedProcess(["/usr/bin/podman", "pull", image], 0)

        with tempfile.TemporaryDirectory() as temp_dir:
            runtime_dir = pathlib.Path(temp_dir) / "runtime"
            auth_file = pathlib.Path(temp_dir) / "auth.json"
            auth_file.write_text('{"auths": {"quay.io": {"auth": "expired"}}}')
            with (
                mock.patch.dict(
                    os.environ,
                    {
                        "MONGO_PODMAN_TASK_ID": "task-stdout-auth",
                        "REGISTRY_AUTH_FILE": str(auth_file),
                    },
                    clear=False,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=runtime_dir,
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        subprocess.CompletedProcess([], 1),
                        subprocess.CompletedProcess([], 0),
                    ],
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_run_container_network_command",
                    side_effect=[rejected, pulled],
                ) as network_command,
            ):
                self.assertTrue(
                    hermetic_container_integration._ensure_linux_container_image(
                        "/usr/bin/podman", image
                    )
                )

        self.assertEqual(2, network_command.call_count)

    def test_image_pull_detects_auth_failure_split_across_streams(self):
        image = "quay.io/mongodb/rbe@sha256:abc123"
        rejected = subprocess.CompletedProcess(
            ["/usr/bin/podman", "pull", image],
            125,
            stdout="invalid username/password: unauthorized",
            stderr="Error: unable to copy from source",
        )
        pulled = subprocess.CompletedProcess(["/usr/bin/podman", "pull", image], 0)

        with tempfile.TemporaryDirectory() as temp_dir:
            runtime_dir = pathlib.Path(temp_dir) / "runtime"
            auth_file = pathlib.Path(temp_dir) / "auth.json"
            auth_file.write_text('{"auths": {"quay.io": {"auth": "expired"}}}')
            with (
                mock.patch.dict(
                    os.environ,
                    {
                        "MONGO_PODMAN_TASK_ID": "task-split-auth",
                        "REGISTRY_AUTH_FILE": str(auth_file),
                    },
                    clear=False,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_runtime_dir",
                    return_value=runtime_dir,
                ),
                mock.patch.object(
                    hermetic_container_integration.subprocess,
                    "run",
                    side_effect=[
                        subprocess.CompletedProcess([], 1),
                        subprocess.CompletedProcess([], 0),
                    ],
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_run_container_network_command",
                    side_effect=[rejected, pulled],
                ) as network_command,
            ):
                self.assertTrue(
                    hermetic_container_integration._ensure_linux_container_image(
                        "/usr/bin/podman", image
                    )
                )

        self.assertEqual(2, network_command.call_count)

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
        self.assertIn(
            "--bes_keywords=MONGO_BUILD_CONTAINERIZED=true",
            run_direct.call_args.args[1],
        )

    def test_container_start_failure_falls_back_to_native_build(self):
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
                side_effect=record_lock,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_action_container",
                return_value=(False, "newuidmap: Operation not permitted"),
            ),
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
        self.assertIn("could not be started or reused", stderr.getvalue())
        self.assertIn("newuidmap: Operation not permitted", stderr.getvalue())
        self.assertIn("running Bazel natively", stderr.getvalue())
        run_direct.assert_called_once_with(
            "/usr/bin/bazel",
            [
                "build",
                "--bes_keywords=MONGO_BUILD_CONTAINERIZED=false",
                "--strategy=MongoInstallRule=local",
                "--symlink_prefix=bazel-",
                "install-dist-test",
            ],
        )

    def test_missing_docker_falls_back_to_native_build(self):
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
            mock.patch.object(
                hermetic_container_integration, "_run_direct", return_value=0
            ) as run_direct,
            redirect_stderr(stderr),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "install-dist-test"],
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
            )

        self.assertEqual(rc, 0)
        self.assertIn("no usable Linux container runtime was found", stderr.getvalue())
        self.assertIn("Docker command not found: docker", stderr.getvalue())
        self.assertIn("running Bazel natively", stderr.getvalue())
        run_direct.assert_called_once_with(
            "/usr/bin/bazel",
            [
                "build",
                "--bes_keywords=MONGO_BUILD_CONTAINERIZED=false",
                "--strategy=MongoInstallRule=local",
                "--symlink_prefix=bazel-",
                "install-dist-test",
            ],
        )

    def test_image_pull_failure_falls_back_to_native_build(self):
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
            mock.patch.object(
                hermetic_container_integration, "_run_direct", return_value=0
            ) as run_direct,
            redirect_stderr(stderr),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "install-dist-test"],
                env={"MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9"},
            )

        self.assertEqual(rc, 0)
        self.assertIn("could not be pulled", stderr.getvalue())
        self.assertIn("running Bazel natively", stderr.getvalue())
        run_direct.assert_called_once_with(
            "/usr/bin/bazel",
            [
                "build",
                "--bes_keywords=MONGO_BUILD_CONTAINERIZED=false",
                "--strategy=MongoInstallRule=local",
                "--symlink_prefix=bazel-",
                "install-dist-test",
            ],
        )

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
            self.assertEqual(on_disk["output_base"], str(output_base))
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


class LinuxCrossModuleOverlayTest(unittest.TestCase):
    def test_materializes_patched_external_modules_without_touching_source(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            source = repo_root / "protobuf"
            source.mkdir()
            source_file = source / "BUILD.bazel"
            source_file.write_text('exports_files(["original"])\n', encoding="utf-8")
            patch_file = repo_root / "protobuf.patch"
            patch_file.write_text(
                """diff --git a/BUILD.bazel b/BUILD.bazel
index 7e2f4c1..0d5b8bc 100644
--- a/BUILD.bazel
+++ b/BUILD.bazel
@@ -1 +1 @@
-exports_files([\"original\"])
+exports_files([\"patched\"])
""",
                encoding="utf-8",
            )
            output_base = repo_root / "output-base"
            overrides = (("protobuf", pathlib.Path("protobuf"), pathlib.Path("protobuf.patch")),)

            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "LINUX_CROSS_MODULE_OVERRIDES",
                    overrides,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_bazel_output_base",
                    return_value=output_base,
                ),
            ):
                args = hermetic_container_integration._linux_cross_module_override_args(
                    ["build", "//:target"], {}, repo_root=repo_root
                )
                second_args = hermetic_container_integration._linux_cross_module_override_args(
                    ["build", "//:target"], {}, repo_root=repo_root
                )

            self.assertEqual(args, second_args)
            self.assertTrue(args[0].startswith("--override_module=protobuf="))
            self.assertEqual(
                source_file.read_text(encoding="utf-8"), 'exports_files(["original"])\n'
            )
            overlay = pathlib.Path(args[0].split("=", 2)[-1])
            self.assertEqual(
                overlay.joinpath("BUILD.bazel").read_text(encoding="utf-8"),
                'exports_files(["patched"])\n',
            )

    def test_does_not_materialize_overlays_for_non_action_commands(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            with mock.patch.object(
                hermetic_container_integration,
                "_bazel_output_base",
                side_effect=AssertionError("output base should not be queried"),
            ):
                self.assertEqual(
                    hermetic_container_integration._linux_cross_module_override_args(
                        ["query", "//:target"], {}, repo_root=repo_root
                    ),
                    [],
                )

    def test_concurrent_overlay_materialization_race_is_benign(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            source = repo_root / "protobuf"
            source.mkdir()
            source_file = source / "BUILD.bazel"
            source_file.write_text('exports_files(["original"])\n', encoding="utf-8")
            patch_file = repo_root / "protobuf.patch"
            patch_file.write_text(
                """diff --git a/BUILD.bazel b/BUILD.bazel
index 7e2f4c1..0d5b8bc 100644
--- a/BUILD.bazel
+++ b/BUILD.bazel
@@ -1 +1 @@
-exports_files([\"original\"])
+exports_files([\"patched\"])
""",
                encoding="utf-8",
            )
            output_base = repo_root / "output-base"
            overrides = (("protobuf", pathlib.Path("protobuf"), pathlib.Path("protobuf.patch")),)

            def concurrent_materialization_rename(src, dst, **kwargs):
                # Simulate another Bazel client completing the same
                # content-addressed materialization between the is_dir() check
                # and the rename, then winning it. Renaming a directory onto an
                # existing non-empty directory raises ENOTEMPTY, not
                # FileExistsError.
                shutil.copytree(src, dst, symlinks=True, dirs_exist_ok=True)
                raise OSError(errno.ENOTEMPTY, "Directory not empty")

            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "LINUX_CROSS_MODULE_OVERRIDES",
                    overrides,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_bazel_output_base",
                    return_value=output_base,
                ),
                mock.patch("os.rename", side_effect=concurrent_materialization_rename),
            ):
                args = hermetic_container_integration._linux_cross_module_override_args(
                    ["build", "//:target"], {}, repo_root=repo_root
                )

            self.assertTrue(args[0].startswith("--override_module=protobuf="))
            overlay = pathlib.Path(args[0].split("=", 2)[-1])
            self.assertEqual(
                overlay.joinpath("BUILD.bazel").read_text(encoding="utf-8"),
                'exports_files(["patched"])\n',
            )
            # The losing client's temporary copy must be cleaned up.
            self.assertEqual(list(overlay.parent.glob(".*.tmp")), [])

    def test_overlay_materialization_unrelated_rename_errors_propagate(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            repo_root = pathlib.Path(temp_dir)
            source = repo_root / "protobuf"
            source.mkdir()
            (source / "BUILD.bazel").write_text('exports_files(["original"])\n', encoding="utf-8")
            patch_file = repo_root / "protobuf.patch"
            patch_file.write_text(
                """diff --git a/BUILD.bazel b/BUILD.bazel
index 7e2f4c1..0d5b8bc 100644
--- a/BUILD.bazel
+++ b/BUILD.bazel
@@ -1 +1 @@
-exports_files([\"original\"])
+exports_files([\"patched\"])
""",
                encoding="utf-8",
            )
            output_base = repo_root / "output-base"
            overrides = (("protobuf", pathlib.Path("protobuf"), pathlib.Path("protobuf.patch")),)

            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "LINUX_CROSS_MODULE_OVERRIDES",
                    overrides,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_bazel_output_base",
                    return_value=output_base,
                ),
                mock.patch(
                    "os.rename",
                    side_effect=OSError(errno.EACCES, "Permission denied"),
                ),
                self.assertRaises(OSError),
            ):
                hermetic_container_integration._linux_cross_module_override_args(
                    ["build", "//:target"], {}, repo_root=repo_root
                )

            # The failed materialization's temporary copy must be cleaned up.
            self.assertEqual(
                list((output_base / "mongo-linux-cross-module-overrides").glob(".*.tmp")),
                [],
            )


if __name__ == "__main__":
    unittest.main()
