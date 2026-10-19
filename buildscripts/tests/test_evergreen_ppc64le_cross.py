"""Check native PPC test dependencies and ARM64 release build scheduling."""

import unittest

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

    def test_smoke_task_runs_linker_regressions_on_arm64(self) -> None:
        commands = self.tasks[self.SMOKE_TASK]["commands"]
        test_commands = [command for command in commands if command.get("func") == "bazel test"]
        self.assertEqual(1, len(test_commands))
        test_command = test_commands[0]
        self.assertEqual(
            {
                "//bazel/toolchains/cc/mongo_linux_cross:cross_toolchain_matrix_test",
                "//bazel/toolchains/cc/mongo_linux_cross:ppc_cross_lld_link_test",
            },
            set(test_command["vars"]["targets"].split()),
        )
        self.assertTrue(test_command["vars"].get("compiling_for_test"))
        self.assertIn("--config=evg", test_command["vars"]["bazel_args"].split())
        self.assertEqual("bazel compile", commands[0]["func"])
        report_index = next(
            index
            for index, command in enumerate(commands)
            if command.get("func") == "enable bazel test report creation"
        )
        self.assertEqual("f_expansions_write", commands[report_index + 1]["func"])
        for static_variant, arm_distro in self.ARM_DISTROS.items():
            for variant_name in (static_variant, static_variant + "-dynamic"):
                entry = next(
                    entry
                    for entry in self.variants[variant_name]["tasks"]
                    if self.SMOKE_TASK
                    in cross_tests._selected_tasks(entry, self.tasks, self.groups)
                )
                with self.subTest(variant=variant_name):
                    self.assertEqual([arm_distro], entry["distros"])


if __name__ == "__main__":
    unittest.main()
