"""Tests for the Evergreen release distro policy."""

import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from bazel.wrapper_hook import bazelrc
from bazel.wrapper_hook import release_distro as under_test

PYTHON_OS = "windows" if os.name == "nt" else "macos" if platform.system() == "Darwin" else "linux"
PYTHON_ARCH = {"aarch64": "arm64", "AMD64": "x86_64"}.get(platform.machine(), platform.machine())
PYTHON_TOOLCHAIN = f"{PYTHON_OS}_{PYTHON_ARCH}"


class ReleaseDistroTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name)
        self.env = {"HOME": str(self.root), "task_id": "task", "distro_id": "release-rhel93-arm64"}

    def test_release_profiles_require_release_distro(self) -> None:
        for profile in under_test.RELEASE_CONFIGS:
            for flags in ([f"--config={profile}"], ["--config", profile]):
                with self.subTest(flags=flags):
                    under_test.check_evergreen_release_distro(
                        ["build", *flags], self.env, self.root
                    )
                    for distro in ("rhel93-arm64", "", "release-", "not-release-rhel93"):
                        with self.assertRaisesRegex(ValueError, "Assign the task or task group"):
                            under_test.check_evergreen_release_distro(
                                ["build", *flags], {**self.env, "distro_id": distro}, self.root
                            )

    def test_missing_distro_fails_in_evergreen(self) -> None:
        with self.assertRaisesRegex(ValueError, "distro_id.*missing"):
            under_test.check_evergreen_release_distro(
                ["build", "--config=public-release"], {"task_id": "task"}, self.root
            )
        with self.assertRaises(ValueError):
            under_test.check_evergreen_release_distro(
                ["build", "--config=public-release"], {}, self.root, evergreen=True
            )

    def test_non_release_and_local_builds_are_allowed(self) -> None:
        under_test.check_evergreen_release_distro(
            ["build", "--config=evg", "--//bazel/config:release=True"],
            {"task_id": "task", "distro_id": "rhel93-arm64"},
            self.root,
        )
        under_test.check_evergreen_release_distro(
            ["build", "--config=public-release"], {}, self.root
        )

    def test_rc_imports_and_multiple_aliases_cannot_bypass_policy(self) -> None:
        (self.root / ".bazelrc").write_text(
            "import %workspace%/release.rc\n"
            "build:publish --config alias\n"
            "build:other --config=public-release-rbe\n"
        )
        (self.root / "release.rc").write_text("common:alias --config=public-release-local\n")
        with self.assertRaisesRegex(ValueError, "public-release-local, public-release-rbe"):
            under_test.check_evergreen_release_distro(
                ["build", "--config=publish", "--config", "other"],
                {**self.env, "distro_id": "regular"},
                self.root,
            )

    def test_unselected_rc_profiles_and_program_arguments_are_ignored(self) -> None:
        (self.root / ".bazelrc").write_text("common:alias --config=public-release\n")
        under_test.check_evergreen_release_distro(
            ["run", "//:program", "--", "--config=alias"],
            {**self.env, "distro_id": "regular"},
            self.root,
        )

    def test_repository_target_is_not_expanded_as_an_argument_file(self) -> None:
        # Bazel treats command-position @ tokens as repository targets, even when
        # a file with that name exists and contains a release profile.
        argument_file = self.root / "release.args"
        argument_file.write_text("--config=public-release\n")
        original_dir = Path.cwd()
        try:
            os.chdir(self.root)
            profiles = under_test.check_evergreen_release_distro(
                ["build", "@release.args"],
                {**self.env, "distro_id": "regular"},
                self.root,
            )
        finally:
            os.chdir(original_dir)
        self.assertEqual((), profiles)

    def test_release_profile_selected_by_rc_defaults_is_checked(self) -> None:
        (self.root / ".bazelrc").write_text("build --config=public-release\n")
        with self.assertRaises(ValueError):
            under_test.check_evergreen_release_distro(
                ["build", "//:mongod"], {**self.env, "distro_id": "regular"}, self.root
            )

    def test_dev_null_preserves_default_and_preceding_explicit_rc_files(self) -> None:
        (self.root / ".bazelrc").write_text("build --config=public-release\n")
        (self.root / "before.rc").write_text("build:publish --config=public-release-local\n")
        (self.root / "after.rc").write_text("build --config=public-release-rbe\n")
        args = [
            "--nosystem_rc",
            "--nohome_rc",
            "--bazelrc=before.rc",
            "--bazelrc=/dev/null",
            "--bazelrc=after.rc",
            "build",
            "--config=publish",
        ]
        self.assertEqual(
            ("public-release", "public-release-local"),
            under_test.check_evergreen_release_distro(args, self.env, self.root),
        )
        with self.assertRaises(ValueError):
            under_test.check_evergreen_release_distro(
                args, {**self.env, "distro_id": "regular"}, self.root
            )
        self.assertEqual(
            (),
            under_test.check_evergreen_release_distro(
                ["--ignore_all_rc_files", *args],
                {**self.env, "distro_id": "regular"},
                self.root,
            ),
        )

    def test_rc_continuations_in_defaults_and_aliases_are_checked(self) -> None:
        for newline in ("\n", "\r\n"):
            continuation = "\\" + newline
            for rc in (
                f"build {continuation} --config=public-release{newline}",
                f"build:publish {continuation} --config=public-release{newline}"
                f"build --config=publish{newline}",
            ):
                with self.subTest(rc=rc):
                    (self.root / ".bazelrc").write_bytes(rc.encode())
                    with self.assertRaises(ValueError):
                        under_test.check_evergreen_release_distro(
                            ["build"], {**self.env, "distro_id": "regular"}, self.root
                        )

    def test_automatic_host_platform_config_checks_imported_release_aliases(self) -> None:
        (self.root / ".bazelrc").write_text(
            "common --enable_platform_specific_config=true\n"
            "import %workspace%/platform.rc\n"
            "build:publish --config=public-release\n"
        )
        for host, config in (("Linux", "linux"), ("Darwin", "macos"), ("Windows", "windows")):
            (self.root / "platform.rc").write_text(f"common:{config} --config=publish\n")
            with self.subTest(host=host), mock.patch.object(platform, "system", return_value=host):
                for command in ("build", "test", "coverage"):
                    with self.assertRaises(ValueError):
                        under_test.check_evergreen_release_distro(
                            [command], {**self.env, "distro_id": "regular"}, self.root
                        )
                for disabled in (
                    "--noenable_platform_specific_config",
                    "--enable_platform_specific_config=false",
                    "--enable_platform_specific_config=0",
                ):
                    self.assertEqual(
                        (),
                        under_test.check_evergreen_release_distro(
                            ["build", disabled], {**self.env, "distro_id": "regular"}, self.root
                        ),
                    )

    def test_platform_config_uses_effective_boolean_and_scope_precedence(self) -> None:
        (self.root / ".bazelrc").write_text(
            "common --enable_platform_specific_config\n"
            "build --noenable_platform_specific_config\n"
            "common:linux --config=public-release\n"
            "common:enable --enable_platform_specific_config=true\n"
        )
        with mock.patch.object(platform, "system", return_value="Linux"):
            self.assertEqual(
                (),
                under_test.check_evergreen_release_distro(
                    ["build"], {**self.env, "distro_id": "regular"}, self.root
                ),
            )
            for args in (
                ["build", "--enable_platform_specific_config"],
                ["build", "--enable_platform_specific_config=Y"],
                ["build", "--config=enable"],
            ):
                with self.subTest(args=args), self.assertRaises(ValueError):
                    under_test.check_evergreen_release_distro(
                        args, {**self.env, "distro_id": "regular"}, self.root
                    )

    def test_platform_options_keep_command_line_precedence(self) -> None:
        (self.root / ".bazelrc").write_text(
            "common --enable_platform_specific_config\n"
            "common:linux --define=stage=platform\n"
            "build --define=stage=build\n"
        )
        with mock.patch.object(platform, "system", return_value="Linux"):
            self.assertEqual(
                [
                    "--enable_platform_specific_config",
                    "--define=stage=platform",
                    "--define=stage=build",
                    "--define=stage=cli",
                ],
                bazelrc._bazel_effective_options(
                    ["--nosystem_rc", "--nohome_rc", "build", "--define=stage=cli"],
                    self.env,
                    self.root,
                ),
            )

    def test_startup_option_values_named_after_commands_are_not_commands(self) -> None:
        (self.root / "build").write_text("build --config=public-release\n")
        for command in ("build", "test", "run", "coverage"):
            for startup in (
                ["--bazelrc", "build"],
                ["--batch", "--bazelrc", "build"],
                ["--output_base", "test", "--bazelrc", "build"],
                ["--host_jvm_args", "run", "--bazelrc", "build"],
            ):
                with self.subTest(command=command, startup=startup):
                    with self.assertRaisesRegex(ValueError, "public-release"):
                        under_test.check_evergreen_release_distro(
                            [*startup, command, "//:target"],
                            {**self.env, "distro_id": "regular"},
                            self.root,
                        )

    def test_build_defaults_and_aliases_apply_to_inheriting_commands(self) -> None:
        for command in (
            "test",
            "run",
            "coverage",
            "aquery",
            "cquery",
            "info",
            "clean",
            "fetch",
            "vendor",
            "mobile-install",
            "print_action",
        ):
            for rc in (
                "build --config=public-release\n",
                "build:publish --config=public-release-local\n",
            ):
                with self.subTest(command=command, rc=rc):
                    (self.root / ".bazelrc").write_text(rc)
                    with self.assertRaisesRegex(ValueError, "public-release"):
                        under_test.check_evergreen_release_distro(
                            [command, "--config=publish", "//:target"],
                            {**self.env, "distro_id": "regular"},
                            self.root,
                        )

    def test_test_defaults_and_aliases_apply_to_inheriting_commands(self) -> None:
        for command in ("coverage", "cquery", "fetch", "vendor"):
            for rc in (
                "test --config=public-release\n",
                "test:publish --config=public-release-rbe\n",
            ):
                with self.subTest(command=command, rc=rc):
                    (self.root / ".bazelrc").write_text(rc)
                    with self.assertRaisesRegex(ValueError, "public-release"):
                        under_test.check_evergreen_release_distro(
                            [command, "--config=publish", "//:target"],
                            {**self.env, "distro_id": "regular"},
                            self.root,
                        )

    def test_inherited_scopes_take_precedence_over_rc_file_order(self) -> None:
        (self.root / ".bazelrc").write_text(
            "coverage --define=stage=coverage\n"
            "test:publish --define=alias=test\n"
            "build --define=stage=build\n"
            "always --define=stage=always\n"
        )
        (self.root / "extra.rc").write_text(
            "build:publish --define=alias=build\n"
            "test --define=stage=test\n"
            "common --define=stage=common\n"
        )
        self.assertEqual(
            [
                "--define=stage=always",
                "--define=stage=common",
                "--define=stage=build",
                "--define=stage=test",
                "--define=stage=coverage",
                "--config=publish",
                "--define=alias=build",
                "--define=alias=test",
                "--define=stage=cli",
            ],
            bazelrc._bazel_effective_options(
                [
                    "--nosystem_rc",
                    "--nohome_rc",
                    "--bazelrc",
                    "extra.rc",
                    "coverage",
                    "--config=publish",
                    "--define=stage=cli",
                ],
                self.env,
                self.root,
            ),
        )

    def test_unrelated_command_scopes_do_not_select_release_profiles(self) -> None:
        (self.root / ".bazelrc").write_text("test --config=public-release\n")
        for command in ("build", "run", "aquery", "query"):
            with self.subTest(command=command):
                self.assertEqual(
                    (),
                    under_test.check_evergreen_release_distro(
                        [command, "//:target"],
                        {**self.env, "distro_id": "regular"},
                        self.root,
                    ),
                )
        (self.root / ".bazelrc").write_text("build:publish --config=public-release\n")
        self.assertEqual(
            (),
            under_test.check_evergreen_release_distro(
                ["query", "--config=publish", "//:target"],
                {**self.env, "distro_id": "regular"},
                self.root,
            ),
        )

    def test_native_wrapper_rejection_is_handled_before_bootstrap(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("wrapper_hook.py")),
                "nonexistent-bazel",
                "build",
                "--config=public-release-local",
                "//:mongod",
            ],
            env={
                **os.environ,
                "task_id": "task",
                "distro_id": "regular",
            },
            capture_output=True,
            text=True,
            check=False,
            timeout=20,
        )
        # Exit code 4 prevents tools/bazel's CI fallback from starting a build.
        self.assertEqual(4, result.returncode, result.stderr)
        self.assertIn("Assign the task or task group", result.stderr)

    @unittest.skipIf(os.name == "nt", "Requires a POSIX shell and symlinks")
    def test_tools_wrapper_does_not_fall_back_after_release_policy_rejection(self) -> None:
        root = self.root / "src"
        (root / "tools").mkdir(parents=True)
        (root / "bazel/wrapper_hook").mkdir(parents=True)
        repository = Path(__file__).resolve().parents[2]
        shutil.copy(repository / "tools/bazel", root / "tools/bazel")
        shutil.copy(
            Path(__file__).with_name("bazel_commands.commands"),
            root / "bazel/wrapper_hook/bazel_commands.commands",
        )
        output = self.root / "cache/execroot/src"
        output.mkdir(parents=True)
        (root / "bazel-src").symlink_to(output, target_is_directory=True)
        python = (
            self.root
            / f"cache/external/_main~setup_mongo_python_toolchains~py_{PYTHON_TOOLCHAIN}/dist/bin/python3"
        )
        python.parent.mkdir(parents=True)
        python.write_text("#!/usr/bin/env bash\nexit 4\n")
        python.chmod(0o755)
        bazel_log = self.root / "bazel.log"
        bazel = self.root / "fake-bazel"
        bazel.write_text(f"#!/usr/bin/env bash\necho build >> '{bazel_log}'\n")
        bazel.chmod(0o755)
        result = subprocess.run(
            ["bash", str(root / "tools/bazel"), "build", "--config=public-release"],
            cwd=root,
            env={
                **os.environ,
                "BAZEL_REAL": str(bazel),
                "BAZELISK_SKIP_WRAPPER": "1",
                "CI": "true",
            },
            capture_output=True,
            text=True,
            check=False,
            timeout=20,
        )
        self.assertEqual(4, result.returncode, result.stderr)
        self.assertFalse(bazel_log.exists(), result.stderr)


if __name__ == "__main__":
    unittest.main()
