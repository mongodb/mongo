"""Unit tests for bazel.wrapper_hook.hermetic_container.cross.windows."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
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
        self.assertIn("--extra_execution_platforms=//bazel/platforms:rhel9_amd64_cross", args)
        self.assertIn("--repo_env=MONGO_LINUX_CROSS_TOOLCHAIN=rhel9_s390x_on_rhel9_x86_64", args)
        self.assertIn("--repo_env=MONGO_WASI_SDK_EXEC_ARCH=x86_64", args)
        self.assertIn("--define=MONGO_IBM_CROSS=1", args)
        self.assertIn("--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=1", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=x86_64", args)
        self.assertIn("--//bazel/config:idl_use_linux_python=True", args)
        self.assertNotIn("--//bazel/config:remote_link=True", args)
        self.assertIn("--spawn_strategy=remote,local", args)
        self.assertNotIn("--spawn_strategy=local", args)
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn("--remote_upload_local_results=false", args)
        for mnemonic in (
            "CppLink",
            "CppArchive",
            "SolibSymlink",
            "ExtractDebugInfo",
            "StripDebugInfo",
            "CcGenerateIntermediateDwp",
            "CcGenerateDwp",
            "GdbGenerateIndex",
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=local", args)
        self.assertGreater(
            args.index("--remote_upload_local_results=false"),
            args.index("--config=linux-s390x-cross-rbe"),
        )
        self.assertNotIn("--strategy=CppLink=remote", args)
        self.assertNotIn("--strategy=CppArchive=remote", args)
        self.assertIn("--strategy=IdlcGenerator=remote", args)
        self.assertIn("--//bazel/config:remote_link=False", args)
        self.assertIn("--test_strategy=standalone", args)
        self.assertIn("--strategy=TestRunner=standalone", args)
        runtime_env = next(
            argument for argument in args if argument.startswith("--test_env=LD_LIBRARY_PATH=")
        )
        # Both canonical repo spellings are emitted: Bazel 9 uses
        # +<extension>+<repo>, Bazel 7 used _main~<extension>~<repo>.
        self.assertIn(
            "+setup_mongo_linux_cross_toolchains_extension+"
            "mongo_linux_cross_toolchain_v5_rhel9_s390x_on_rhel9_x86_64/target/stow/gcc-v5/lib64",
            runtime_env,
        )
        self.assertIn(
            "_main~setup_mongo_linux_cross_toolchains_extension~"
            "mongo_linux_cross_toolchain_v5_rhel9_s390x_on_rhel9_x86_64/target/stow/gcc-v5/lib64",
            runtime_env,
        )
        self.assertGreater(
            args.index("--strategy=TestRunner=standalone"),
            args.index("--config=linux-s390x-cross-rbe"),
        )

    def test_plans_linux_s390x_cross_rbe_remote_link_host_args(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "build",
                "--config=linux-s390x-cross-rbe",
                "--config=remote_link",
                "install-dist-test",
            ],
            {},
            containers={"rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}},
        )

        self.assertIn("--//bazel/config:remote_link=False", args)
        self.assertIn("--strategy=CppLink=local", args)
        self.assertIn("--strategy=CppArchive=local", args)
        self.assertIn("--strategy=SolibSymlink=local", args)
        self.assertIn("--remote_upload_local_results=false", args)
        self.assertNotIn("--strategy=CppLink=remote", args)

    def test_linux_cross_rbe_local_output_actions_use_host_container(self):
        args = hermetic_container_integration._linux_host_container_action_args(
            hermetic_container_integration._linux_cross_rbe_host_args(
                [
                    "build",
                    "--config=linux-s390x-cross-rbe",
                    "install-dist-test",
                ],
                {},
                containers={"rhel9": {"container-url": "docker://example.invalid/rbe@sha256:123"}},
            ),
            {},
            remote_compile_only=True,
        )

        for mnemonic in (
            "CppLink",
            "CppArchive",
            "CppArchiveDist",
            "SolibSymlink",
            "ExtractDebugInfo",
            "StripDebugInfo",
            "ObjcopyEmbedData",
            "CcGenerateIntermediateDwp",
            "CcGenerateDwp",
            "GdbGenerateIndex",
            "GdbApplyIndex",
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=persistent-container,local", args)
                self.assertNotIn(f"--strategy={mnemonic}=remote", args)

        for mnemonic in (
            "CppCompile",
            "Rustc",
            "RustcMetadata",
            *(
                mnemonic
                for mnemonic in hermetic_container_integration.LINUX_CROSS_REMOTE_COMPILE_MNEMONICS
                if mnemonic != "Genrule"
            ),
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=remote", args)
        # Genrule is remote-first but tolerates no-remote-tagged genrules
        # (e.g. the resmoke TSS test list) via the native container fallback.
        self.assertIn("--strategy=Genrule=remote,persistent-container,local", args)
        self.assertIn(
            r"--strategy_regexp=.*Linking .*\[for tool\].*=remote",
            args,
        )
        self.assertIn(r"--strategy_regexp=.*Linking .*\.wasm.*=remote", args)

    def test_linux_cross_rbe_runs_native_wasm_aot_and_remote_generators(self):
        args = hermetic_container_integration._linux_host_container_action_args(
            hermetic_container_integration._linux_cross_rbe_host_args(
                [
                    "build",
                    "--config=linux-s390x-cross-rbe",
                    "install-dist-test",
                ],
                {},
                containers={"rhel9": {"container-url": "docker://example.invalid/rbe@sha256:123"}},
            ),
            {},
            remote_compile_only=True,
        )

        # The AOT rule supplies a target-built s390x Wasmtime executable.
        # Bindgen tools still run on the foreign execution platform.
        self.assertIn("--strategy=WasmAotCompile=persistent-container,local", args)
        self.assertNotIn("--strategy=WasmAotCompile=remote", args)
        for mnemonic in ("WitBindgenC", "RustWasmBindgen"):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=remote", args)
                self.assertNotIn(f"--strategy={mnemonic}=persistent-container,local", args)
        # WASI compile and link actions still use the execution-architecture SDK.
        self.assertIn("--strategy=CppCompile=remote", args)
        self.assertIn(r"--strategy_regexp=.*Linking .*\.wasm.*=remote", args)

    def test_linux_cross_rbe_local_test_strategy_overrides_remote_test_config(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "test",
                "--config=linux-s390x-cross-rbe",
                "--config=remote_test",
                "//src/mongo/stdx:stdx_test",
            ],
            {},
            containers={"rhel9": {"container-url": "docker://example.invalid/rbe@sha256:123"}},
        )

        self.assertGreater(
            args.index("--strategy=TestRunner=standalone"),
            args.index("--config=remote_test"),
        )

    def test_linux_cross_rbe_coverage_uses_local_test_strategy(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "coverage",
                "--config=linux-ppc64le-cross-rbe",
                "--config=remote_test",
                "//src/mongo/stdx:stdx_test",
            ],
            {},
            containers={"rhel9": {"container-url": "docker://example.invalid/rbe@sha256:123"}},
        )

        self.assertGreater(
            args.index("--strategy=TestRunner=standalone"),
            args.index("--config=remote_test"),
        )

    def test_linux_cross_rbe_remote_mode_is_compile_only(self):
        args = [
            "build",
            "--config=linux-ppc64le-rhel10-cross-rbe",
            "--config=remote_test",
            "install-dist-test",
        ]
        host_args = hermetic_container_integration._linux_host_container_action_args(
            hermetic_container_integration._linux_cross_rbe_host_args(
                args,
                {},
                containers={"rhel9": {"container-url": "docker://example.invalid/rbe@sha256:123"}},
            ),
            {},
            remote_compile_only=True,
        )

        for mnemonic in (
            "CppCompile",
            "Rustc",
            "RustcMetadata",
            *(
                mnemonic
                for mnemonic in hermetic_container_integration.LINUX_CROSS_REMOTE_COMPILE_MNEMONICS
                if mnemonic != "Genrule"
            ),
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=remote", host_args)
        # Genrule is remote-first but must tolerate targets tagged no-remote
        # (the resmoke TSS test list genrule needs host credentials and
        # network) instead of failing strategy selection.
        self.assertIn("--strategy=Genrule=remote,persistent-container,local", host_args)
        # Keep the failure-prone execution-platform tools covered explicitly so
        # removing them from the policy tuple cannot silently regress cross builds.
        for mnemonic in (
            "WheelBuild",
            "WheelInstall",
            "UpbAmalgamation",
            "EmbedEditionDefaults",
            "PycrossTargetEnvironment",
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=remote", host_args)
        # The host-side tar metadata writer is explicitly tagged no-remote by
        # rules_pkg and must remain local even though other execution-platform
        # tools (including Genrule, WheelBuild, and WheelInstall) are remote in cross mode.
        self.assertIn("--strategy=PyWriteBuildData=persistent-container,local", host_args)
        self.assertNotIn("--strategy=PyWriteBuildData=remote", host_args)
        # Signing and public-key generation are release/provenance actions. They must
        # use the native IBM persistent container even when compilation is remote.
        for mnemonic in ("EmbedPublicKeyHeader", "GpgExportArmored", "GpgSign"):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=persistent-container,local", host_args)
                self.assertNotIn(f"--strategy={mnemonic}=remote", host_args)
        # These host-only Python generators are constrained to the native host
        # execution platform, so their local persistent-container actions never
        # attempt to execute a foreign Python runtime on IBM hosts.
        for mnemonic in (
            "MongoPrettyPrinterTestCreation",
            "CertificateGenerator",
            "ExtractCertificateGenerationYear",
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=persistent-container,local", host_args)
                self.assertNotIn(f"--strategy={mnemonic}=remote", host_args)
        for mnemonic in ("CppLink", "CppArchive", "TestRunner"):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(
                    f"--strategy={mnemonic}="
                    + ("persistent-container,local" if mnemonic != "TestRunner" else "local"),
                    host_args,
                )
        self.assertIn("--spawn_strategy=local", host_args)
        self.assertTrue(
            any(
                argument.startswith("--modify_execution_info=^(MongoInstallRule|")
                for argument in host_args
            )
        )

    def test_linux_cross_rbe_release_local_mode_uses_native_toolchain(self):
        for config_name, native_platform, foreign_platform in (
            (
                "linux-s390x-rhel8-cross-rbe",
                "//bazel/platforms:rhel8_s390x",
                "//bazel/platforms:rhel9_amd64_cross",
            ),
            (
                "linux-s390x-rhel9-cross-rbe",
                "//bazel/platforms:rhel9_s390x",
                "//bazel/platforms:rhel9_amd64_cross",
            ),
            (
                "linux-ppc64le-rhel10-cross-rbe",
                "//bazel/platforms:rhel10_ppc64le",
                "//bazel/platforms:rhel9_amd64_cross",
            ),
        ):
            with self.subTest(config=config_name):
                args = [
                    "build",
                    f"--config={config_name}",
                    "--config=public-release-local",
                    "install-dist-test",
                    *hermetic_container_integration.RELEASE_LOCAL_SAFETY_SUFFIX,
                ]
                config = hermetic_container_integration._linux_cross_rbe_config(
                    args, env={}, repo_root=hermetic_container_integration.REPO_ROOT
                )
                self.assertIsNotNone(config)
                host_args = hermetic_container_integration._linux_host_container_action_args(
                    hermetic_container_integration._linux_cross_rbe_local_host_args(args, config),
                    {},
                    force_local=True,
                )

                self.assertIn(f"--platforms={native_platform}", host_args)
                self.assertIn("--extra_execution_platforms=", host_args)
                self.assertIn(f"--extra_execution_platforms={native_platform}", host_args)
                self.assertIn("--extra_toolchains=", host_args)
                self.assertIn("--remote_executor=", host_args)
                self.assertIn("--noremote_accept_cached", host_args)
                self.assertIn("--modify_execution_info=.*=+no-cache", host_args)
                self.assertNotIn("--remote_cache=", host_args)
                self.assertIn("--define=MONGO_IBM_CROSS=0", host_args)
                self.assertIn("--linker=auto", host_args)
                self.assertNotIn(f"--extra_execution_platforms={foreign_platform}", host_args)
                for mnemonic in (
                    "CppCompile",
                    "Rustc",
                    "CppLink",
                    "CppArchive",
                    "Genrule",
                    "WheelInstall",
                    "PyWriteBuildData",
                ):
                    with self.subTest(mnemonic=mnemonic):
                        self.assertIn(
                            f"--strategy={mnemonic}=persistent-container,local", host_args
                        )
                        self.assertNotIn(f"--strategy={mnemonic}=remote", host_args)
                self.assertEqual(
                    tuple(
                        host_args[
                            -len(hermetic_container_integration.RELEASE_LOCAL_SAFETY_SUFFIX) :
                        ]
                    ),
                    hermetic_container_integration.RELEASE_LOCAL_SAFETY_SUFFIX,
                )

    def test_linux_cross_local_release_clears_cross_process_environment(self):
        process_env = hermetic_container_integration._linux_cross_local_process_env(
            {
                "MONGO_LINUX_CROSS_TOOLCHAIN": "rhel9_s390x_on_rhel9_x86_64",
                "MONGO_WASI_SDK_EXEC_ARCH": "x86_64",
                "MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH": "x86_64",
                "MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE": "quay.io/example/foreign@sha256:abc",
            }
        )

        for name in hermetic_container_integration.LINUX_CROSS_LOCAL_RELEASE_ENV_VARS:
            with self.subTest(name=name):
                self.assertNotIn(name, process_env)

    def test_linux_cross_local_release_does_not_reuse_cross_container_image(self):
        local_env = hermetic_container_integration._linux_cross_local_container_env(
            {"MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE": "quay.io/example/foreign@sha256:abc"},
        )

        self.assertNotIn("MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE", local_env)
        self.assertNotIn("MONGO_HERMETIC_CONTAINER_IMAGE", local_env)

    def test_linux_cross_rbe_forwards_task_scoped_podman_repo_env(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"

            def which(command: str) -> str | None:
                return "/usr/bin/podman" if command == "podman" else None

            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_task_root",
                    return_value=task_root,
                ),
                mock.patch.object(
                    hermetic_container_integration.shutil,
                    "which",
                    side_effect=which,
                ),
            ):
                args = hermetic_container_integration._linux_cross_rbe_host_args(
                    [
                        "build",
                        "--config=linux-ppc64le-rhel9-cross-rbe",
                        "install-dist-test",
                    ],
                    {"MONGO_PODMAN_TASK_ID": "task/123"},
                    containers={
                        "rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}
                    },
                )

            runtime_dir = task_root / (
                f"{hermetic_container_integration.PODMAN_RUNTIME_DIR_PREFIX}{os.getuid()}"
            )
            storage_config = (
                task_root
                / f"{hermetic_container_integration.PODMAN_STORAGE_DIR_PREFIX}{os.getuid()}"
                / "storage.conf"
            )
            self.assertIn(
                f"--repo_env=CONTAINERS_STORAGE_CONF={storage_config}",
                args,
            )
            containers_config = runtime_dir / "containers.conf"
            self.assertIn(f"--repo_env=CONTAINERS_CONF={containers_config}", args)
            self.assertIn(f"--repo_env=XDG_RUNTIME_DIR={runtime_dir}", args)
            self.assertIn(f"--repo_env=TMPDIR={runtime_dir}", args)
            self.assertIn('driver = "overlay"', storage_config.read_text(encoding="utf-8"))

    def test_linux_cross_rbe_required_podman_uses_task_scoped_repo_env(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"

            def which(command: str) -> str | None:
                return {"docker": "/usr/bin/docker", "podman": "/usr/bin/podman"}.get(command)

            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_task_root",
                    return_value=task_root,
                ),
                mock.patch.object(
                    hermetic_container_integration.shutil,
                    "which",
                    side_effect=which,
                ),
                mock.patch.object(
                    hermetic_container_integration,
                    "_is_podman_docker_shim",
                    side_effect=AssertionError(
                        "Docker must not be inspected when Podman is required"
                    ),
                ) as is_shim,
            ):
                args = hermetic_container_integration._linux_cross_rbe_host_args(
                    ["build", "--config=linux-s390x-rhel9-cross-rbe", "install-dist-test"],
                    {
                        "MONGO_PODMAN_REQUIRED": "true",
                        "MONGO_PODMAN_TASK_ID": "task_123",
                    },
                    containers={
                        "rhel9": {"container-url": "docker://example.invalid/rbe@sha256:abc"}
                    },
                )

        self.assertTrue(any(arg.startswith("--repo_env=CONTAINERS_STORAGE_CONF=") for arg in args))
        self.assertTrue(any(arg.startswith("--repo_env=CONTAINERS_CONF=") for arg in args))
        self.assertTrue(any(arg.startswith("--repo_env=XDG_RUNTIME_DIR=") for arg in args))
        is_shim.assert_not_called()

    def test_linux_cross_rbe_podman_task_id_does_not_override_container_opt_out(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "build",
                "--config=linux-ppc64le-rhel9-cross-rbe",
                "install-dist-test",
            ],
            {
                "MONGO_LINUX_CONTAINER_ACTIONS": "0",
                "MONGO_PODMAN_TASK_ID": "task/123",
            },
            containers={"rhel9": {"container-url": "docker://example.invalid/rbe@sha256:abc"}},
        )

        self.assertFalse(any(arg.startswith("--repo_env=CONTAINERS_STORAGE_CONF=") for arg in args))
        self.assertFalse(any(arg.startswith("--repo_env=XDG_RUNTIME_DIR=") for arg in args))

    def test_linux_cross_rbe_preserves_explicit_podman_repo_env(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "build",
                "--config=linux-s390x-rhel9-cross-rbe",
                "install-dist-test",
            ],
            {
                "CONTAINERS_STORAGE_CONF": "/tmp/explicit-podman/storage.conf",
                "XDG_RUNTIME_DIR": "/tmp/explicit-podman/runtime",
            },
            containers={"rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}},
        )

        self.assertIn(
            "--repo_env=CONTAINERS_STORAGE_CONF=/tmp/explicit-podman/storage.conf",
            args,
        )
        self.assertIn("--repo_env=XDG_RUNTIME_DIR=/tmp/explicit-podman/runtime", args)

    def test_linux_cross_rbe_forwards_explicit_podman_auth_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            auth_file = pathlib.Path(temp_dir) / "auth.json"
            auth_file.write_text("{}", encoding="utf-8")
            args = hermetic_container_integration._linux_cross_rbe_host_args(
                [
                    "build",
                    "--config=linux-s390x-rhel9-cross-rbe",
                    "install-dist-test",
                ],
                {"REGISTRY_AUTH_FILE": str(auth_file)},
                containers={"rhel9": {"container-url": "docker://example.invalid/rbe@sha256:abc"}},
            )

        self.assertIn(f"--repo_env=REGISTRY_AUTH_FILE={auth_file}", args)

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

        self.assertIn("--extra_execution_platforms=//bazel/platforms:rhel9_arm64_cross", args)
        self.assertIn("--remote_default_exec_properties=Pool=default", args)
        self.assertIn("--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=aarch64", args)
        self.assertIn("--repo_env=MONGO_WASI_SDK_EXEC_ARCH=aarch64", args)

    def test_plans_linux_s390x_rhel10_cross_rbe_host_args(self):
        args = hermetic_container_integration._linux_cross_rbe_host_args(
            [
                "build",
                "--config=linux-s390x-rhel10-cross-rbe",
                "install-dist-test",
            ],
            {},
            containers={"rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}},
        )

        self.assertIn("--extra_execution_platforms=//bazel/platforms:rhel9_amd64_cross", args)
        self.assertIn("--repo_env=MONGO_LINUX_CROSS_TOOLCHAIN=rhel10_s390x_on_rhel9_x86_64", args)

    def test_linux_cross_rbe_process_env_uses_execution_platform(self):
        config = hermetic_container_integration.LinuxCrossRBEConfig(
            target_arch="s390x",
            target_distro="rhel10",
            exec_arch="x86_64",
            exec_distro="rhel9",
        )

        process_env = hermetic_container_integration._linux_cross_rbe_process_env(
            config,
            {
                "MONGO_LINUX_CROSS_TOOLCHAIN": "stale_s390x_on_s390x",
                "MONGO_WASI_SDK_EXEC_ARCH": "s390x",
            },
        )

        self.assertEqual(
            process_env["MONGO_LINUX_CROSS_TOOLCHAIN"],
            "rhel10_s390x_on_rhel9_x86_64",
        )
        self.assertEqual(process_env["MONGO_WASI_SDK_EXEC_ARCH"], "x86_64")

    def test_linux_cross_rbe_process_env_forwards_task_scoped_podman_state(self):
        config = hermetic_container_integration.LinuxCrossRBEConfig(
            target_arch="ppc64le",
            target_distro="rhel9",
            exec_arch="x86_64",
            exec_distro="rhel9",
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            task_root = pathlib.Path(temp_dir) / "mongo-linux-podman-task-task_123"
            runtime_dir = task_root / (
                f"{hermetic_container_integration.PODMAN_RUNTIME_DIR_PREFIX}{os.getuid()}"
            )

            def which(command: str) -> str | None:
                return "/usr/bin/podman" if command == "podman" else None

            with (
                mock.patch.object(
                    hermetic_container_integration,
                    "_podman_task_root",
                    return_value=task_root,
                ),
                mock.patch.object(
                    hermetic_container_integration.shutil,
                    "which",
                    side_effect=which,
                ),
                mock.patch.dict(
                    hermetic_container_integration.os.environ,
                    {
                        "TMPDIR": str(runtime_dir),
                        "TMP": str(runtime_dir),
                        "TEMP": str(runtime_dir),
                    },
                ),
            ):
                process_env = hermetic_container_integration._linux_cross_rbe_process_env(
                    config,
                    {"MONGO_PODMAN_TASK_ID": "task_123"},
                )

        storage_config = (
            task_root
            / f"{hermetic_container_integration.PODMAN_STORAGE_DIR_PREFIX}{os.getuid()}"
            / "storage.conf"
        )
        self.assertEqual(process_env["CONTAINERS_STORAGE_CONF"], str(storage_config))
        self.assertEqual(process_env["CONTAINERS_CONF"], str(runtime_dir / "containers.conf"))
        self.assertEqual(process_env["XDG_RUNTIME_DIR"], str(runtime_dir))
        # Podman's task runtime is forwarded through --repo_env and actual
        # Podman subprocess environments, but must not become Bazel's global
        # temporary directory. Local host actions can outlive Podman recovery,
        # so inheriting this directory would leave linux-sandbox with a stale
        # bind-mount source.
        self.assertNotEqual(process_env.get("TMPDIR"), str(runtime_dir))
        self.assertNotEqual(process_env.get("TMP"), str(runtime_dir))
        self.assertNotEqual(process_env.get("TEMP"), str(runtime_dir))

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
                [
                    "query",
                    "--config=linux-s390x-cross-rbe",
                    "--config=remote_link",
                    "//src/mongo/stdx:stdx_test",
                ],
                env={},
            )

        self.assertEqual(rc, 0)
        docker_status.assert_not_called()
        run_direct.assert_called_once()
        self.assertEqual(run_direct.call_args.args[0], "/usr/bin/bazel")
        host_args = run_direct.call_args.args[1]
        self.assertIn("--remote_executor=grpcs://sodalite.cluster.engflow.com", host_args)
        self.assertIn("--strategy=CppCompile=remote", host_args)
        self.assertIn("--strategy=CppLink=local", host_args)
        self.assertIn("--extra_execution_platforms=//bazel/platforms:rhel9_amd64_cross", host_args)

    def test_run_hermetic_container_routes_release_cross_build_locally(self):
        containers = {"rhel9": {"container-url": "docker://example.invalid/native@sha256:123"}}
        stdout = StringIO()

        with (
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "load_remote_execution_containers",
                return_value=containers,
            ),
            redirect_stdout(stdout),
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                [
                    "build",
                    "--config=linux-s390x-rhel9-cross-rbe",
                    "--config=public-release-local",
                    "install-dist-test",
                ],
                env={
                    "MONGO_HERMETIC_CONTAINER_DRY_RUN": "1",
                    "MONGO_HERMETIC_CONTAINER_DISTRO": "rhel9",
                    "MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE": "docker://example.invalid/foreign@sha256:456",
                },
            )

        self.assertEqual(rc, 0)
        dry_run = json.loads(stdout.getvalue().splitlines()[-1])
        host_args = dry_run["linux_cross_host_rbe"]["args"]
        self.assertIn("--platforms=//bazel/platforms:rhel9_s390x", host_args)
        self.assertIn("--remote_executor=", host_args)
        self.assertIn("--modify_execution_info=.*=+no-cache", host_args)
        self.assertNotIn("--remote_cache=", host_args)
        self.assertIn("--strategy=CppCompile=persistent-container,local", host_args)
        self.assertNotIn("--strategy=CppCompile=remote", host_args)
        self.assertNotIn("foreign@sha256:456", json.dumps(dry_run))
        self.assertNotIn("--override_module=protobuf=", json.dumps(dry_run))
        self.assertNotIn("--override_module=grpc=", json.dumps(dry_run))

    def test_run_hermetic_container_uses_native_container_for_linux_cross_link(self):
        containers = {"rhel9": {"container-url": "docker://quay.io/mongodb/rbe@sha256:abc123"}}
        container_config = {
            "image": "quay.io/mongodb/rbe@sha256:abc123",
            "container_name": "mongo_linux_action_rhel9_aarch64_example",
        }

        with (
            tempfile.TemporaryDirectory() as temp_dir,
            mock.patch.object(
                hermetic_container_integration.platform, "system", return_value="Linux"
            ),
            mock.patch.object(
                hermetic_container_integration,
                "load_remote_execution_containers",
                return_value=containers,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_select_linux_container_runtime",
                return_value=("/usr/bin/podman", ""),
            ) as select_runtime,
            mock.patch.object(
                hermetic_container_integration,
                "_linux_host_container_config",
                return_value=container_config,
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_container_image",
                return_value=True,
            ) as ensure_image,
            mock.patch.object(
                hermetic_container_integration,
                "_bazel_output_base",
                return_value=pathlib.Path(temp_dir) / "output-base",
            ),
            mock.patch.object(
                hermetic_container_integration,
                "_write_linux_container_actions_config_unlocked",
                return_value=(
                    pathlib.Path(temp_dir) / "output-base" / "config.json",
                    container_config,
                ),
            ) as write_config,
            mock.patch.object(
                hermetic_container_integration,
                "_ensure_linux_action_container",
                return_value=(True, ""),
            ) as ensure_container,
            mock.patch.object(
                hermetic_container_integration,
                "_run_direct",
                return_value=0,
            ) as run_direct,
        ):
            rc = hermetic_container_integration.run_hermetic_container(
                "/usr/bin/bazel",
                ["build", "--config=linux-s390x-cross-rbe", "install-dist-test"],
                env={
                    "MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE": ("quay.io/example/foreign@sha256:456"),
                },
            )

        self.assertEqual(rc, 0)
        select_runtime.assert_called_once()
        ensure_image.assert_called_once_with("/usr/bin/podman", "quay.io/mongodb/rbe@sha256:abc123")
        write_config.assert_called_once()
        self.assertEqual(
            write_config.call_args.args[1]["HERMETIC_CONTAINER_DOCKER_COMMAND"],
            "/usr/bin/podman",
        )
        self.assertNotIn(
            "MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE",
            write_config.call_args.args[1],
        )
        self.assertNotIn("MONGO_HERMETIC_CONTAINER_IMAGE", write_config.call_args.args[1])
        ensure_container.assert_called_once()
        run_direct.assert_called_once()
        host_args = run_direct.call_args.args[1]
        self.assertIn("--strategy=CppCompile=remote", host_args)
        self.assertTrue(any(arg.startswith("--override_module=protobuf=") for arg in host_args))
        self.assertTrue(any(arg.startswith("--override_module=grpc=") for arg in host_args))
        self.assertIn("--strategy=CppLink=persistent-container,local", host_args)
        self.assertNotIn("--strategy=CppLink=remote", host_args)
        self.assertIn("--remote_upload_local_results=false", host_args)
        for mnemonic in (
            "CppArchive",
            "CppArchiveDist",
            "ExtractDebugInfo",
            "StripDebugInfo",
            "ObjcopyEmbedData",
            "CcGenerateIntermediateDwp",
            "CcGenerateDwp",
            "GdbGenerateIndex",
            "GdbApplyIndex",
        ):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(f"--strategy={mnemonic}=persistent-container,local", host_args)
                self.assertNotIn(f"--strategy={mnemonic}=remote", host_args)
        self.assertIn("--strategy=ConfigHeaderGen=remote", host_args)
        self.assertIn("--strategy=WasmAotCompile=persistent-container,local", host_args)
        self.assertNotIn("--strategy=WasmAotCompile=remote", host_args)
        self.assertIn(
            r"--strategy_regexp=.*Linking .*\[for tool\].*=remote",
            host_args,
        )
        self.assertNotIn(r"--strategy_regexp=.*Linking .*wasm.*=remote", host_args)
        self.assertGreater(
            host_args.index(r"--strategy_regexp=.*Linking .*\[for tool\].*=remote"),
            host_args.index("--strategy=CppLink=persistent-container,local"),
        )

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
