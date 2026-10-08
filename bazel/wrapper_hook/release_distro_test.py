"""Tests for the Evergreen release distro policy."""

import contextlib
import os
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
        status = self.root / "status"
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
                "MONGO_BAZEL_WRAPPER_STATUS": str(status),
            },
            capture_output=True,
            text=True,
            check=False,
            timeout=20,
        )
        # The handled response prevents tools/bazel's CI fallback from starting a build.
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("handled=1\nexit_code=1\n", status.read_text())
        self.assertIn("Assign the task or task group", result.stderr)


if __name__ == "__main__":
    unittest.main()
