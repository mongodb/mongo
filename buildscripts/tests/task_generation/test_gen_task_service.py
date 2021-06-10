"""Unit tests for gen_task_service.py."""

import unittest
from unittest.mock import MagicMock

from shrub.v2 import BuildVariant

import buildscripts.task_generation.gen_task_service as under_test
from buildscripts.task_generation.task_types.fuzzer_tasks import FuzzerGenTaskService

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


def build_mock_fuzzer_params(multi_version=None, use_large_distro=None, add_to_display=True,
                             large_distro_name=None):
    return under_test.FuzzerGenTaskParams(
        task_name="task name",
        variant="build variant",
        suite="resmoke suite",
        num_files=10,
        num_tasks=5,
        resmoke_args="args for resmoke",
        npm_command="jstestfuzz",
        jstestfuzz_vars="vars for jstestfuzz",
        continue_on_failure=True,
        resmoke_jobs_max=5,
        should_shuffle=True,
        timeout_secs=100,
        require_multiversion=multi_version,
        use_large_distro=use_large_distro,
        add_to_display_task=add_to_display,
        large_distro_name=large_distro_name,
        config_location="config location",
    )


def build_mocked_service():
    return under_test.GenTaskService(
        evg_api=MagicMock(),
        gen_task_options=MagicMock(),
        gen_config=MagicMock(),
        resmoke_gen_task_service=MagicMock(),
        multiversion_gen_task_service=MagicMock(),
        fuzzer_gen_task_service=FuzzerGenTaskService(),
    )


class TestGenerateFuzzerTask(unittest.TestCase):
    def test_fuzzer_tasks_should_be_generated(self):
        mock_params = build_mock_fuzzer_params()
        build_variant = BuildVariant("mock build variant")
        service = build_mocked_service()

        fuzzer_task = service.generate_fuzzer_task(mock_params, build_variant)

        self.assertEqual(fuzzer_task.task_name, mock_params.task_name)
        self.assertEqual(len(fuzzer_task.sub_tasks), mock_params.num_tasks)

        self.assertEqual(len(build_variant.tasks), mock_params.num_tasks)

        display_tasks = list(build_variant.display_tasks)
        self.assertEqual(len(display_tasks), 1)
        self.assertEqual(display_tasks[0].display_name, mock_params.task_name)
        self.assertEqual(len(display_tasks[0].execution_tasks), mock_params.num_tasks)

    def test_fuzzer_for_large_distro_tasks_should_be_generated_on_large(self):
        mock_distro = "my large distro"
        mock_params = build_mock_fuzzer_params(use_large_distro=True, large_distro_name=mock_distro)
        build_variant = BuildVariant("mock build variant")
        service = build_mocked_service()
        service.gen_task_options.large_distro_name = mock_distro

        service.generate_fuzzer_task(mock_params, build_variant)

        fuzzer_config = build_variant.as_dict()
        self.assertTrue(all(mock_distro in task["distros"] for task in fuzzer_config["tasks"]))

    def test_fuzzer_tasks_should_not_be_added_to_display_group_when_specified(self):
        mock_params = build_mock_fuzzer_params(add_to_display=False)
        build_variant = BuildVariant("mock build variant")
        service = build_mocked_service()

        fuzzer_task = service.generate_fuzzer_task(mock_params, build_variant)

        self.assertEqual(fuzzer_task.task_name, mock_params.task_name)
        self.assertEqual(len(fuzzer_task.sub_tasks), mock_params.num_tasks)

        self.assertEqual(len(build_variant.tasks), mock_params.num_tasks)

        display_tasks = list(build_variant.display_tasks)
        self.assertEqual(len(display_tasks), 0)


class TestGetDistro(unittest.TestCase):
    def test_default_distro_should_be_used_if_use_large_distro_not_set(self):
        service = build_mocked_service()

        distros = service._get_distro("build variant", use_large_distro=False,
                                      large_distro_name=None)

        self.assertIsNone(distros)

    def test_large_distro_should_be_used_if_use_large_distro_is_set(self):
        mock_distro = "my large distro"
        service = build_mocked_service()

        distros = service._get_distro("build variant", use_large_distro=True,
                                      large_distro_name=mock_distro)

        self.assertEqual(distros, [mock_distro])

    def test_a_missing_large_distro_should_throw_error(self):
        service = build_mocked_service()

        with self.assertRaises(ValueError):
            service._get_distro("build variant", use_large_distro=True, large_distro_name=None)

    def test_a_missing_large_distro_can_be_ignored(self):
        build_variant = "my build variant"
        service = build_mocked_service()
        service.gen_config.build_variant_large_distro_exceptions = {
            "some other build", build_variant, "build 3"
        }

        distros = service._get_distro(build_variant, use_large_distro=True, large_distro_name=None)

        self.assertIsNone(distros)
