"""Check native PPC test dependencies and ARM64 release build scheduling."""

import shutil
import tempfile
import unittest
from pathlib import Path

from bazel.wrapper_hook import bazelrc
from buildscripts.tests import test_evergreen_s390x_cross as cross_tests


class Ppc64leCrossSchedulingTest(cross_tests.S390xCrossSchedulingTest):
    STATIC_VARIANTS = {
        "enterprise-rhel-81-ppc64le": "rhel81-power8-large",
        "enterprise-rhel-9-ppc64le": "rhel9-power-large",
        "enterprise-rhel-10-ppc64le": "rhel10-power-large",
    }
    ARM_DISTROS = {
        "enterprise-rhel-81-ppc64le": "rhel8.8-arm64-m8g-4xlarge",
        "enterprise-rhel-9-ppc64le": "rhel93-arm64-m8g-4xlarge",
        "enterprise-rhel-10-ppc64le": "rhel10.0-arm64-large",
    }
    SMOKE_TASK = "ppc64le_cross_toolchain_smoke"
    CROSS_PACKAGE_GROUP = "compile_all_and_package_serial_no_unittests_TG"
    INTEGRATION_GROUP = "compile_integration_and_test_no_audit_parallel_stream_TG"
    HAS_MONGOR = False

    def test_cross_configs_select_ppc64le_without_architecture_define(self) -> None:
        target_platforms = {
            "enterprise-rhel-81-ppc64le": "rhel8_ppc64le_cross",
            "enterprise-rhel-9-ppc64le": "rhel9_ppc64le_cross",
            "enterprise-rhel-10-ppc64le": "rhel10_ppc64le_cross",
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            shutil.copy(cross_tests.REPO_ROOT / ".bazelrc", root)
            for variant_name, target_platform in target_platforms.items():
                config = self.variants[variant_name]["expansions"]["mongo_ppc64le_cross_config"]
                options = bazelrc._bazel_effective_options(
                    ["--nosystem_rc", "--nohome_rc", "build", f"--config={config}"],
                    env={},
                    repo_root=root,
                )
                architecture_defines = [
                    option for option in options if option.startswith("--define=MONGO_ARCH=")
                ]
                with self.subTest(variant=variant_name):
                    self.assertEqual([], architecture_defines)
                    self.assertIn(f"--platforms=//bazel/platforms:{target_platform}", options)

    def test_native_dbtest_hosts_match_waterfall_release_flags(self) -> None:
        for static_name, native_distro in self.STATIC_VARIANTS.items():
            for variant_name in (static_name, static_name + "-dynamic"):
                variant = self.variants[variant_name]
                entry = next(
                    entry
                    for entry in variant["tasks"]
                    if entry["name"] == "compile_run_and_archive_dbtest_TG"
                )
                for task_name in ("compile_dbtest", "archive_dbtest"):
                    flags = self._release_flags(variant_name, task_name, is_patch=False)
                    expected_distro = (
                        "release-" + native_distro
                        if "--config=public-release-local" in flags
                        else native_distro
                    )
                    with self.subTest(variant=variant_name, task=task_name):
                        self.assertEqual([expected_distro], entry.get("distros", variant["run_on"]))

    def test_selected_tasks_exist_on_v83(self) -> None:
        for variant_name in self.STATIC_VARIANTS:
            for entry in self.variants[variant_name]["tasks"]:
                if not entry["name"].startswith("."):
                    self.assertTrue(
                        entry["name"] in self.tasks or entry["name"] in self.groups,
                        (variant_name, entry["name"]),
                    )


if __name__ == "__main__":
    unittest.main()
