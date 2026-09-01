"""Regression tests for custom-build version-expansion command ordering."""

import unittest
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPONENT_ROOT = REPO_ROOT / "etc" / "evergreen_yml_components"


def _load_component(relative_path: str) -> dict:
    with (COMPONENT_ROOT / relative_path).open() as component_file:
        return yaml.safe_load(component_file)


def _find_task(relative_path: str, task_name: str) -> list[dict]:
    component = _load_component(relative_path)
    return next(task["commands"] for task in component["tasks"] if task.get("name") == task_name)


def _function_commands(function_name: str) -> list[dict]:
    definitions = _load_component("definitions.yml")
    return definitions["functions"][function_name]


def _func_index(commands: list[dict], function_name: str) -> int:
    return next(
        index for index, command in enumerate(commands) if command.get("func") == function_name
    )


def _display_name_index(commands: list[dict], display_name: str) -> int:
    return next(
        index
        for index, command in enumerate(commands)
        if command.get("display_name") == display_name
    )


def _expansion_update_indices(commands: list[dict], file_name: str) -> list[int]:
    return [
        index
        for index, command in enumerate(commands)
        if command.get("command") == "expansions.update"
        and command.get("params", {}).get("file") == file_name
    ]


def _script_index(commands: list[dict], script_name: str) -> int:
    return next(
        index
        for index, command in enumerate(commands)
        if script_name in command.get("params", {}).get("args", [])
    )


class CustomBuildVersionOrderingTest(unittest.TestCase):
    def test_shared_version_expansions_do_not_require_the_venv(self):
        shared_commands = _function_commands("get and apply version expansions")
        custom_commands = _function_commands("set custom build version override")

        self.assertNotIn("set_version_override.sh", str(shared_commands))
        self.assertIn("set_version_override.sh", str(custom_commands))

    def test_custom_version_override_tasks_set_up_the_venv_first(self):
        task_locations = (
            ("tasks/compile_tasks.yml", "publish_packages", "set up venv"),
            ("tasks/compile_tasks.yml", "crypt_push", "set up venv"),
            ("tasks/compile_tasks_nightly.yml", "sign_windows_msi", "set up venv"),
            ("tasks/compile_tasks_nightly.yml", "test_windows_msi", "set up venv"),
            ("tasks/compile_tasks_nightly.yml", "push", "set up venv"),
            (
                "tasks/compile_tasks_shared.yml",
                "crypt_lib_package",
                "do non-compile setup",
            ),
            ("custom_builds/tasks.yml", "custom_build_deliver", "set up venv"),
        )

        for relative_path, task_name, setup_function in task_locations:
            with self.subTest(task=task_name):
                commands = _find_task(relative_path, task_name)
                self.assertLess(
                    _func_index(commands, setup_function),
                    _func_index(commands, "set custom build version override"),
                )

    def test_package_test_functions_are_called_after_venv_setup(self):
        task_locations = (
            ("tasks/misc_tasks.yml", "test_packages", "run package test"),
            ("tasks/misc_tasks.yml", "test_crypt_packages", "run crypt package test"),
        )

        for relative_path, task_name, package_test_function in task_locations:
            with self.subTest(task=task_name):
                commands = _find_task(relative_path, task_name)
                self.assertLess(
                    _func_index(commands, "set up venv"),
                    _func_index(commands, package_test_function),
                )
                package_test_commands = _function_commands(package_test_function)
                apply_indices = _expansion_update_indices(
                    package_test_commands, "src/version_expansions.yml"
                )
                self.assertEqual(2, len(apply_indices))
                override_index = _script_index(
                    package_test_commands, "src/evergreen/custom_builds/set_version_override.sh"
                )
                self.assertGreater(override_index, apply_indices[0])
                self.assertLess(override_index, apply_indices[-1])

    def test_packager_functions_apply_custom_version_before_packaging(self):
        packager_functions = (
            ("run packager.py", "packager py_run sh"),
            ("run packager_crypt.py", "packager crypt py_run sh"),
        )

        for function_name, packager_display_name in packager_functions:
            with self.subTest(function=function_name):
                commands = _function_commands(function_name)
                self.assertIn("set_version_override.sh", str(commands))
                self.assertLess(
                    _display_name_index(
                        commands, "set custom build version override (if configured)"
                    ),
                    _display_name_index(commands, packager_display_name),
                )


if __name__ == "__main__":
    unittest.main()
