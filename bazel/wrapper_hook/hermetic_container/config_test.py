"""Unit tests for bazel.wrapper_hook.hermetic_container.config."""

from __future__ import annotations

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


if __name__ == "__main__":
    unittest.main()
