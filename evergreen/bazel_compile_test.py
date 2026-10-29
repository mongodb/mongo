"""Exercise release distro checks with the real Evergreen compile wrapper."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[1]


class BazelCompileTest(unittest.TestCase):
    def run_compile(
        self, *, distro: str | None, **expansions: str
    ) -> tuple[subprocess.CompletedProcess, str]:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            src = root / "src"
            evergreen = src / "evergreen"
            evergreen.mkdir(parents=True)
            shutil.copy(REPO_ROOT / "evergreen/bazel_compile.sh", evergreen)
            shutil.copy(REPO_ROOT / "evergreen/bazel_evergreen_shutils.sh", evergreen)
            (src / ".bazelrc").write_text("common:publish --config=public-release-local\n")
            # Keep the real option composition and policy; replace host/server operations.
            (evergreen / "prelude.sh").write_text("activate_venv() { :; }\n")
            with (evergreen / "bazel_evergreen_shutils.sh").open("a") as helpers:
                helpers.write(
                    "\n"
                    "bazel_evergreen_shutils::export_ssl_paths_if_needed() { :; }\n"
                    "bazel_evergreen_shutils::ensure_server_and_print_pid() { :; }\n"
                    "bazel_evergreen_shutils::bazel_get_binary_path() { echo fake-bazel; }\n"
                    "bazel_evergreen_shutils::retry_bazel_cmd() { echo build >> build.log; }\n"
                    "bazel_evergreen_shutils::write_last_engflow_link() { :; }\n"
                )
            env = {
                **os.environ,
                "PYTHONPATH": str(REPO_ROOT),
                "python": sys.executable,
                "task_name": "archive_dist_test",
                "project": "mongodb-mongo-master",
                "is_patch": "true",
                "targets": "archive-dist-stripped",
                **expansions,
            }
            env.pop("distro_id", None)
            if distro is not None:
                env["distro_id"] = distro
            result = subprocess.run(
                ["bash", str(evergreen / "bazel_compile.sh")],
                cwd=root,
                env=env,
                text=True,
                capture_output=True,
                check=False,
                timeout=20,
            )
            log = src / "build.log"
            if result.returncode == 0:
                captured_distro = src / ".bazel_provenance_build_invocation.distro_id"
                self.assertEqual(f"{distro}\n", captured_distro.read_text())
            return result, log.read_text() if log.exists() else ""

    def test_release_profiles_fail_before_build_on_regular_or_missing_distro(self) -> None:
        for profile in ("public-release", "public-release-local", "public-release-rbe", "publish"):
            for flag in (f"--config={profile}", f"--config {profile}"):
                for source in (
                    "bazel_args",
                    "bazel_compile_flags",
                    "task_compile_flags",
                    "patch_compile_flags",
                ):
                    with self.subTest(flag=flag, source=source):
                        result, log = self.run_compile(distro="rhel93-arm64", **{source: flag})
                        self.assertNotEqual(0, result.returncode)
                        self.assertEqual("", log)
                        self.assertIn("Assign the task or task group", result.stderr)
        for distro in (None, ""):
            result, log = self.run_compile(distro=distro, bazel_args="--config=public-release")
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("", log)
            self.assertIn("distro_id='<missing>'", result.stderr)

    def test_all_release_profiles_allow_release_distro(self) -> None:
        for profile in ("public-release", "public-release-local", "public-release-rbe"):
            with self.subTest(profile=profile):
                result, log = self.run_compile(
                    distro="release-rhel93-arm64", bazel_args=f"--config={profile}"
                )
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertEqual("build\n", log)

    def test_auto_selected_release_profiles_are_checked(self) -> None:
        for expansions in (
            {"MONGO_VERSION_OVERRIDE": "9.0.0"},
            {"is_release": "true", "release_rbe": "true"},
            {"is_release": "true", "is_patch": "false", "push_bucket": "downloads"},
            {"project": "mongodb-mongo-master-nightly", "release_rbe": "true"},
            {
                "project": "mongodb-mongo-master-nightly",
                "is_patch": "false",
                "push_bucket": "downloads",
            },
        ):
            with self.subTest(expansions=expansions):
                result, log = self.run_compile(distro="regular", **expansions)
                self.assertNotEqual(0, result.returncode)
                self.assertEqual("", log)

    def test_master_builds_do_not_auto_select_release_profiles(self) -> None:
        for is_patch in ("true", "false"):
            for release_rbe in ("true", "false"):
                with self.subTest(patch=is_patch, release_rbe=release_rbe):
                    result, log = self.run_compile(
                        distro="regular",
                        is_patch=is_patch,
                        release_rbe=release_rbe,
                        push_bucket="downloads",
                    )
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.assertEqual("build\n", log)

    def test_explicit_release_on_master_still_requires_release_distro(self) -> None:
        for distro in ("regular", "release-build-host"):
            with self.subTest(distro=distro):
                result, log = self.run_compile(distro=distro, MONGO_VERSION_OVERRIDE="9.0.0")
                self.assertEqual(
                    distro.startswith("release-"), result.returncode == 0, result.stderr
                )
                self.assertEqual("build\n" if distro.startswith("release-") else "", log)

    def test_release_projects_still_select_release_profiles(self) -> None:
        for project in ("mongodb-mongo-v8.0", "mongo-release", "mongodb-mongo-master-nightly"):
            for release_rbe in ("true", "false"):
                with self.subTest(project=project, release_rbe=release_rbe):
                    result, log = self.run_compile(
                        distro="regular",
                        project=project,
                        is_patch="false",
                        release_rbe=release_rbe,
                        push_bucket="downloads",
                    )
                    self.assertNotEqual(0, result.returncode)
                    self.assertEqual("", log)
                    self.assertIn("Assign the task or task group", result.stderr)

    def test_non_release_build_allows_regular_distro(self) -> None:
        result, log = self.run_compile(distro="regular", bazel_args="--config=evg --release=True")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("build\n", log)

    def test_cross_build_uses_executing_distro(self) -> None:
        flags = "--config=linux-s390x-rhel9-cross-rbe-arm64 --config=public-release-local"
        for distro in ("rhel93-arm64-m8g-4xlarge", "release-rhel93-arm64-m8g-4xlarge"):
            with self.subTest(distro=distro):
                result, log = self.run_compile(distro=distro, bazel_compile_flags=flags)
                self.assertEqual(
                    distro.startswith("release-"), result.returncode == 0, result.stderr
                )
                self.assertEqual("build\n" if distro.startswith("release-") else "", log)

    def test_ibm_release_variant_assignments_in_patch_and_waterfall(self) -> None:
        config = yaml.safe_load(
            (REPO_ROOT / "etc/evergreen_yml_components/variants/ibm/test_release.yml").read_text()
        )
        for variant in config["buildvariants"]:
            if not variant["expansions"]["has_packages"]:
                continue
            build_entries = [
                task
                for task in variant["tasks"]
                if task["name"].startswith(
                    (
                        "compile_all_and_package_",
                        "compile_and_archive_dist_test_",
                        "compile_test_and_package_",
                        "compile_test_serial_",
                    )
                )
                or task["name"] == "crypt_create_lib"
            ]
            self.assertTrue(build_entries, variant["name"])
            for entry in build_entries:
                for is_patch in ("true", "false"):
                    with self.subTest(variant=variant["name"], task=entry["name"], patch=is_patch):
                        result, log = self.run_compile(
                            distro=entry["distros"][0],
                            project="mongodb-mongo-master-nightly",
                            bazel_compile_flags=variant["expansions"]["bazel_compile_flags"],
                            push_bucket=variant["expansions"]["push_bucket"],
                            release_rbe=str(variant["expansions"]["release_rbe"]).lower(),
                            is_patch=is_patch,
                        )
                        self.assertEqual(0, result.returncode, result.stderr)
                        self.assertEqual("build\n", log)

    def test_development_libraries_do_not_select_release_profiles(self) -> None:
        config = yaml.safe_load(
            (REPO_ROOT / "etc/evergreen_yml_components/tasks/compile_tasks.yml").read_text()
        )
        tasks = {task["name"]: task for task in config["tasks"]}
        variants = yaml.safe_load(
            (REPO_ROOT / "etc/evergreen_yml_components/variants/ibm/test_release.yml").read_text()
        )["buildvariants"]
        for name in ("compile_mongor", "stitch_support_create_lib"):
            task = tasks[name]
            compile_vars = next(
                c["vars"] for c in task["commands"] if c.get("func") == "bazel compile"
            )
            for release_rbe in ("true", "false"):
                for is_patch in ("true", "false"):
                    with self.subTest(task=name, release_rbe=release_rbe, patch=is_patch):
                        result, log = self.run_compile(
                            distro="regular",
                            project="mongodb-mongo-master-nightly",
                            push_bucket="downloads",
                            release_rbe=release_rbe,
                            is_patch=is_patch,
                            **{
                                key: str(value).lower() if isinstance(value, bool) else str(value)
                                for key, value in compile_vars.items()
                            },
                        )
                        self.assertEqual(0, result.returncode, result.stderr)
                        self.assertEqual("build\n", log)
                        self.assertNotIn("Release configuration(s)", result.stderr)
            for variant in variants:
                for entry in variant["tasks"]:
                    if entry["name"] == name:
                        self.assertFalse(
                            entry["distros"][0].startswith("release-"), variant["name"]
                        )

    def test_ibm_test_only_variants_in_patch_and_waterfall(self) -> None:
        config = yaml.safe_load(
            (REPO_ROOT / "etc/evergreen_yml_components/variants/ibm/test_release.yml").read_text()
        )
        variants = [v for v in config["buildvariants"] if not v["expansions"]["has_packages"]]
        self.assertEqual(6, len(variants))
        for variant in variants:
            expansions = variant["expansions"]
            for entry in variant["tasks"]:
                distro = entry["distros"][0]
                with self.subTest(variant=variant["name"], task=entry["name"]):
                    self.assertFalse(distro.startswith("release-"))
                    for project in ("mongodb-mongo-master-nightly", "mongodb-mongo-v8.0"):
                        for is_patch in ("true", "false"):
                            result, log = self.run_compile(
                                distro=distro,
                                project=project,
                                bazel_compile_flags=expansions["bazel_compile_flags"],
                                push_bucket=expansions["push_bucket"],
                                release_rbe=str(expansions["release_rbe"]).lower(),
                                is_patch=is_patch,
                            )
                            self.assertEqual(0, result.returncode, result.stderr)
                            self.assertEqual("build\n", log)
                            self.assertNotIn("Release configuration(s)", result.stderr)

                    # Test-only variant settings must not bypass explicit release checks.
                    for release_flags in (
                        {"MONGO_VERSION_OVERRIDE": "9.0.0"},
                        {"bazel_args": "--config=public-release"},
                    ):
                        result, log = self.run_compile(
                            distro=distro,
                            project="mongodb-mongo-master-nightly",
                            bazel_compile_flags=expansions["bazel_compile_flags"],
                            push_bucket=expansions["push_bucket"],
                            release_rbe=str(expansions["release_rbe"]).lower(),
                            is_patch="false",
                            **release_flags,
                        )
                        self.assertNotEqual(0, result.returncode)
                        self.assertEqual("", log)
                        self.assertIn("Assign the task or task group", result.stderr)


if __name__ == "__main__":
    unittest.main()
