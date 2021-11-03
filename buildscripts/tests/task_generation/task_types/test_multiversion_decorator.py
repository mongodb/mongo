"""Unit tests for multiversion_decorator.py"""

import unittest
from unittest.mock import patch

import inject

from buildscripts.task_generation.resmoke_proxy import ResmokeProxyService
from buildscripts.task_generation.task_types import multiversion_decorator as under_test
from buildscripts.task_generation.task_types.multiversion_decorator import MultiversionDecoratorParams


# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access,no-value-for-parameter
def build_mock_params():
    return MultiversionDecoratorParams(task="dummy_task", base_suite="dummy_suite",
                                       variant="dummy_variant", num_tasks=5)


def build_mock_sub_tasks():
    mock_tasks = set()
    commands = [
        under_test.FunctionCall(under_test.CONFIGURE_EVG_CREDENTIALS),
    ]
    for i in range(5):
        mock_task = under_test.Task(f"task_name_{i}", commands)
        mock_tasks.add(mock_task)
    return mock_tasks


class TestDecorateFuzzerGenTask(unittest.TestCase):
    def setUp(self) -> None:
        def dependencies(binder: inject.Binder) -> None:
            binder.bind(ResmokeProxyService, ResmokeProxyService())

        inject.clear_and_configure(dependencies)

    def tearDown(self) -> None:
        inject.clear()

    def run_test(self, fixture_type):
        mock_params = build_mock_params()
        mock_sub_tasks = build_mock_sub_tasks()
        multiversion_decorator = under_test.MultiversionGenTaskDecorator()
        expected_num_tasks = mock_params.num_tasks * len(multiversion_decorator.old_versions) * len(
            multiversion_decorator._get_versions_combinations(fixture_type))

        with patch.object(multiversion_decorator,
                          "_get_suite_fixture_type") as mock_get_suite_fixture_type:
            mock_get_suite_fixture_type.return_value = fixture_type

            sub_tasks = multiversion_decorator.decorate_tasks_with_dynamically_generated_files(
                mock_sub_tasks, mock_params)

            self.assertEqual(len(sub_tasks), expected_num_tasks)
            self.assertTrue(
                all(sub_task.commands[3].name == under_test.DO_MULTIVERSION_SETUP
                    for sub_task in sub_tasks))

    def test_decorate_fuzzer_shell_fixture(self):
        self.run_test(under_test._SuiteFixtureType.SHELL)

    def test_decorate_fuzzer_replset_fixture(self):
        self.run_test(under_test._SuiteFixtureType.REPL)

    def test_decorate_fuzzer_sharded_cluster_fixture(self):
        self.run_test(under_test._SuiteFixtureType.SHARD)

    def test_decorate_fuzzer_unrecognized_fixture(self):
        self.run_test(under_test._SuiteFixtureType.OTHER)
