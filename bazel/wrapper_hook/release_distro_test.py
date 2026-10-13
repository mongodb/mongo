"""Tests for the Evergreen release distro policy."""

import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from bazel.wrapper_hook import release_distro as under_test


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
        with contextlib.chdir(self.root):
            profiles = under_test.check_evergreen_release_distro(
                ["build", "@release.args"],
                {**self.env, "distro_id": "regular"},
                self.root,
            )
        self.assertEqual((), profiles)

    def test_release_profile_selected_by_rc_defaults_is_checked(self) -> None:
        (self.root / ".bazelrc").write_text("build --config=public-release\n")
        with self.assertRaises(ValueError):
            under_test.check_evergreen_release_distro(
                ["build", "//:mongod"], {**self.env, "distro_id": "regular"}, self.root
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
            / "cache/external/_main~setup_mongo_python_toolchains~py_host/dist/bin/python3"
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
