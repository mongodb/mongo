"""Regression tests for native test scheduling on s390x cross-build variants."""

import os
import shlex
import subprocess
import unittest
from pathlib import Path
from typing import Any

import yaml

COMPONENT_ROOT = Path(__file__).resolve().parents[2] / "etc" / "evergreen_yml_components"
REPO_ROOT = COMPONENT_ROOT.parents[1]
STATIC_VARIANTS = {
    "enterprise-rhel-83-s390x": "rhel83-zseries-large",
    "enterprise-rhel-9-s390x": "rhel9-zseries-large",
    "enterprise-rhel-10-s390x": "rhel10-zseries-large",
}
ARM_DISTROS = {
    "enterprise-rhel-83-s390x": "rhel8.8-arm64-m8g-4xlarge",
    "enterprise-rhel-9-s390x": "rhel93-arm64-m8g-4xlarge",
    "enterprise-rhel-10-s390x": "rhel10.0-arm64-large",
}


def _load_component(path: Path) -> dict[str, Any]:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def _selected_tasks(
    entry: dict[str, Any], tasks: dict[str, Any], groups: dict[str, Any]
) -> list[str]:
    """Expand task groups and the tag/name intersections used by these variants."""
    selector = entry["name"]
    if selector in groups:
        return groups[selector]["tasks"]
    if selector in tasks:
        return [selector]

    def matches(name: str, task: dict[str, Any]) -> bool:
        tags = task.get("tags", [])
        for term in selector.split():
            negated = term.startswith("!")
            term = term.removeprefix("!")
            present = term[1:] in tags if term.startswith(".") else term == name
            if present == negated:
                return False
        return True

    return [name for name, task in tasks.items() if matches(name, task)]


def _dependencies(dependencies: list[str | dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        {"name": dependency} if isinstance(dependency, str) else dependency
        for dependency in dependencies
    ]


class S390xCrossSchedulingTest(unittest.TestCase):
    STATIC_VARIANTS = STATIC_VARIANTS
    ARM_DISTROS = ARM_DISTROS
    SMOKE_TASK = "s390x_cross_toolchain_smoke"
    CROSS_PACKAGE_GROUP = "compile_s390x_cross_and_package_TG"
    INTEGRATION_GROUP = "compile_integration_and_test_parallel_stream_TG"

    @classmethod
    def setUpClass(cls) -> None:
        cls.tasks: dict[str, Any] = {}
        cls.groups: dict[str, Any] = {}
        for path in (COMPONENT_ROOT / "tasks").rglob("*.yml"):
            component = _load_component(path)
            cls.tasks.update({task["name"]: task for task in component.get("tasks", [])})
            cls.groups.update({group["name"]: group for group in component.get("task_groups", [])})
        cls.variants = {
            variant["name"]: variant
            for variant in _load_component(COMPONENT_ROOT / "variants/ibm/test_release.yml")[
                "buildvariants"
            ]
        }
        cls.functions = _load_component(COMPONENT_ROOT / "definitions.yml")["functions"]

    def _fetches_venv(self, commands: list[dict[str, Any]]) -> bool:
        for command in commands:
            if command.get("display_name") == "fetch venv":
                return True
            if function_name := command.get("func"):
                function = self.functions.get(function_name, [])
                function_commands = [function] if isinstance(function, dict) else function
                if self._fetches_venv(function_commands):
                    return True
        return False

    def _release_flags(self, variant_name: str, task_name: str, *, is_patch: bool) -> list[str]:
        compile_command = next(
            command
            for command in self.tasks[task_name]["commands"]
            if command.get("func") in ("bazel compile", "bazel test")
        )
        expansions = self.variants[variant_name]["expansions"] | compile_command.get("vars", {})
        env = os.environ.copy()
        for key in (
            "MONGO_VERSION_OVERRIDE",
            "release_rbe",
            "push_bucket",
            "compiling_for_test",
            "release_binary_only",
        ):
            env.pop(key, None)
        env.update(
            {
                key: str(value).lower() if isinstance(value, bool) else str(value)
                for key, value in expansions.items()
            }
        )
        env["is_patch"] = str(is_patch).lower()
        result = subprocess.run(
            [
                "bash",
                "-c",
                'source "$1"; bazel_evergreen_shutils::maybe_release_flag "--config=evg"',
                "release-flags-test",
                str(REPO_ROOT / "evergreen/bazel_evergreen_shutils.sh"),
            ],
            env=env,
            capture_output=True,
            text=True,
            check=True,
        )
        return shlex.split(result.stdout)

    def test_critical_selectors_preserve_task_dependencies(self) -> None:
        for variant_name in self.STATIC_VARIANTS:
            variant = self.variants[variant_name]
            all_selected_tasks = set()
            for entry in variant["tasks"]:
                if not entry["name"].startswith((".development_critical", ".all_variants")):
                    continue
                selected_tasks = _selected_tasks(entry, self.tasks, self.groups)
                all_selected_tasks.update(selected_tasks)
                for task_name in selected_tasks:
                    task = self.tasks[task_name]
                    effective = _dependencies(entry.get("depends_on", task.get("depends_on", [])))
                    with self.subTest(variant=variant_name, task=task_name):
                        for dependency in _dependencies(task.get("depends_on", [])):
                            self.assertIn(dependency, effective)
                        self.assertIn({"name": "create_venv"}, effective)
            self.assertTrue(all_selected_tasks, variant_name)

    def test_all_native_venv_consumers_wait_for_the_native_producer(self) -> None:
        for variant_name in self.STATIC_VARIANTS:
            variant = self.variants[variant_name]
            checked_consumers = set()
            for entry in variant["tasks"]:
                for task_name in _selected_tasks(entry, self.tasks, self.groups):
                    task = self.tasks[task_name]
                    if not self._fetches_venv(task.get("commands", [])):
                        continue
                    checked_consumers.add(task_name)
                    effective = _dependencies(entry.get("depends_on", task.get("depends_on", [])))
                    with self.subTest(variant=variant_name, task=task_name):
                        self.assertIn({"name": "create_venv"}, effective)
            self.assertIn("unittest_shell_hang_analyzer_gen", checked_consumers)
            self.assertIn("generate_buildid_to_debug_symbols_mapping", checked_consumers)

    def test_hang_analyzer_preserves_dependencies_and_waits_for_native_venv(self) -> None:
        task_name = "unittest_shell_hang_analyzer_gen"
        task = self.tasks[task_name]
        for variant_name in self.STATIC_VARIANTS:
            entry = next(
                entry
                for entry in self.variants[variant_name]["tasks"]
                if entry["name"] == task_name
            )
            effective = _dependencies(entry.get("depends_on", task["depends_on"]))
            with self.subTest(variant=variant_name):
                for dependency in _dependencies(task["depends_on"]):
                    self.assertIn(dependency, effective)
                self.assertIn({"name": "create_venv"}, effective)

    def test_explicit_native_compile_tasks_use_large_hosts(self) -> None:
        task_names = {
            "compile_mongor",
            "crypt_create_lib",
            "crypt_lib_package",
            "stitch_support_create_lib",
            "stitch_support_install_and_run_tests",
        }
        for variant_name, large_distro in self.STATIC_VARIANTS.items():
            variant = self.variants[variant_name]
            for task_name in task_names:
                entry = next(entry for entry in variant["tasks"] if entry["name"] == task_name)
                with self.subTest(variant=variant_name, task=task_name):
                    self.assertEqual([large_distro], entry.get("distros", variant["run_on"]))

    def test_static_variants_retain_native_integration_coverage(self) -> None:
        expected = {
            "compile_integration_test",
            "integration_tests_standalone",
            "integration_tests_replset",
            "integration_tests_replset_ssl_auth",
            "integration_tests_sharded",
        }
        if self.INTEGRATION_GROUP == "compile_integration_and_test_parallel_stream_TG":
            expected.update(
                {"integration_tests_standalone_audit", "integration_tests_standalone_grpc"}
            )
        for variant_name, large_distro in self.STATIC_VARIANTS.items():
            variant = self.variants[variant_name]
            scheduled = {
                task_name: entry.get("distros", variant["run_on"])
                for entry in variant["tasks"]
                for task_name in _selected_tasks(entry, self.tasks, self.groups)
            }
            for task_name in expected:
                with self.subTest(variant=variant_name, task=task_name):
                    self.assertEqual([large_distro], scheduled.get(task_name))

    def test_smoke_build_waits_for_version_expansions(self) -> None:
        self.assertIn(
            {"name": "version_expansions_gen", "variant": "generate-tasks-for-version"},
            self.tasks[self.SMOKE_TASK].get("depends_on", []),
        )

    def test_smoke_build_is_only_for_testing(self) -> None:
        compile_command = next(
            command
            for command in self.tasks[self.SMOKE_TASK]["commands"]
            if command.get("func") == "bazel compile"
        )
        self.assertTrue(compile_command["vars"].get("compiling_for_test"))

    def test_only_distributable_cross_builds_use_release_distros(self) -> None:
        for static_name, arm_distro in self.ARM_DISTROS.items():
            static_variant = self.variants[static_name]
            dynamic_variant = self.variants[static_name + "-dynamic"]
            with self.subTest(variant=dynamic_variant["name"]):
                self.assertFalse(dynamic_variant["expansions"].get("compiling_for_test", False))
                self.assertTrue(dynamic_variant["expansions"].get("release_binary_only"))
                self.assertFalse(dynamic_variant["expansions"].get("release_rbe"))
                for group_name in (
                    self.SMOKE_TASK + "_TG",
                    "compile_and_archive_dist_test_TG",
                ):
                    entry = next(
                        entry for entry in dynamic_variant["tasks"] if entry["name"] == group_name
                    )
                    self.assertEqual([arm_distro], entry["distros"])
            with self.subTest(variant=static_name):
                self.assertFalse(static_variant["expansions"].get("compiling_for_test", False))
                entry = next(
                    entry
                    for entry in static_variant["tasks"]
                    if entry["name"] == self.CROSS_PACKAGE_GROUP
                )
                self.assertEqual(["release-" + arm_distro], entry["distros"])

    def test_dynamic_server_builds_preserve_release_binary_flags_on_waterfall(self) -> None:
        for static_name in self.STATIC_VARIANTS:
            for task_name in (
                "archive_dist_test",
                "archive_dist_test_debug",
                "compile_dbtest",
                "archive_dbtest",
            ):
                with self.subTest(variant=static_name, task=task_name):
                    self.assertEqual(
                        ["--config=evg", "--config=public-release-local"],
                        self._release_flags(static_name, task_name, is_patch=False),
                    )
                    self.assertEqual(
                        ["--config=evg", "--config=release-binary"],
                        self._release_flags(static_name + "-dynamic", task_name, is_patch=False),
                    )
                    self.assertEqual(
                        ["--config=evg"],
                        self._release_flags(static_name + "-dynamic", task_name, is_patch=True),
                    )

    def test_test_only_tasks_do_not_gain_release_binary_flags(self) -> None:
        for static_name in self.STATIC_VARIANTS:
            for variant_name in (static_name, static_name + "-dynamic"):
                for task_name in (self.SMOKE_TASK, "compile_libraries_for_unittests"):
                    with self.subTest(variant=variant_name, task=task_name):
                        self.assertEqual(
                            ["--config=evg"],
                            self._release_flags(variant_name, task_name, is_patch=False),
                        )
            for group in range(1, 9):
                with self.subTest(variant=static_name + "-dynamic", group=group):
                    self.assertEqual(
                        ["--config=evg"],
                        self._release_flags(
                            static_name + "-dynamic", f"unit_test_group{group}", is_patch=False
                        ),
                    )

    def test_release_binary_config_shares_public_release_binary_settings(self) -> None:
        configs: dict[str, list[str]] = {}
        for line in (REPO_ROOT / ".bazelrc").read_text(encoding="utf-8").splitlines():
            if line.startswith("common:"):
                config, *flags = shlex.split(line)
                configs.setdefault(config.removeprefix("common:"), []).extend(flags)
        self.assertEqual(
            {
                "--//bazel/config:release=True",
                "--//bazel/config:extensions_signature_verification_secure=True",
                "--stamp",
            },
            set(configs["release-binary"]),
        )
        self.assertIn("--config=release-binary", configs["public-release"])


if __name__ == "__main__":
    unittest.main()
