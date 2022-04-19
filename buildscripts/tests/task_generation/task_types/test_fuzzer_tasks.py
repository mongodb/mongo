"""Unit tests for fuzzer_tasks.py"""

import unittest

import inject

import buildscripts.task_generation.task_types.fuzzer_tasks as under_test
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyService

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


def build_mock_fuzzer_params(jstestfuzz_vars="vars for jstestfuzz", npm_command="jstestfuzz"):
    return under_test.FuzzerGenTaskParams(
        task_name="task name",
        variant="build variant",
        suite="resmoke suite",
        num_files=10,
        num_tasks=5,
        resmoke_args="args for resmoke",
        npm_command=npm_command,
        jstestfuzz_vars=jstestfuzz_vars,
        continue_on_failure=True,
        resmoke_jobs_max=5,
        should_shuffle=True,
        timeout_secs=100,
        require_multiversion_setup=False,
        use_large_distro=None,
        large_distro_name="large distro",
        config_location="config_location",
        dependencies={"task_dependency"},
    )


class TestFuzzerGenTaskParams(unittest.TestCase):
    def test_jstestfuzz_params_should_be_generated(self):
        params = build_mock_fuzzer_params()

        jstestfuzz_vars = params.jstestfuzz_params()

        self.assertEqual(params.npm_command, jstestfuzz_vars["npm_command"])
        self.assertIn(f"--numGeneratedFiles {params.num_files}", jstestfuzz_vars["jstestfuzz_vars"])
        self.assertIn(params.jstestfuzz_vars, jstestfuzz_vars["jstestfuzz_vars"])

    def test_jstestfuzz_params_should_handle_no_vars(self):
        params = build_mock_fuzzer_params(jstestfuzz_vars=None)

        self.assertNotIn("None", params.jstestfuzz_params()["jstestfuzz_vars"])

    def test_get_task_name_with_no_version(self):
        params = build_mock_fuzzer_params()

        self.assertEqual(params.task_name, params.task_name)

    def test_get_non_sharded_resmoke_args(self):
        params = build_mock_fuzzer_params()

        self.assertEqual(params.resmoke_args, params.get_resmoke_args())


class TestGenerateTasks(unittest.TestCase):
    def setUp(self) -> None:
        def dependencies(binder: inject.Binder) -> None:
            binder.bind(ResmokeProxyService, ResmokeProxyService())

        inject.clear_and_configure(dependencies)

    def tearDown(self) -> None:
        inject.clear()

    def test_fuzzer_tasks_are_generated(self):
        mock_params = build_mock_fuzzer_params()
        fuzzer_service = under_test.FuzzerGenTaskService()

        fuzzer_task = fuzzer_service.generate_tasks(mock_params)

        self.assertEqual(fuzzer_task.task_name, mock_params.task_name)
        self.assertEqual(len(fuzzer_task.sub_tasks), mock_params.num_tasks)


class TestBuildFuzzerSubTask(unittest.TestCase):
    def setUp(self) -> None:
        def dependencies(binder: inject.Binder) -> None:
            binder.bind(ResmokeProxyService, ResmokeProxyService())

        inject.clear_and_configure(dependencies)

    def tearDown(self) -> None:
        inject.clear()

    def test_sub_task_should_be_built_correct(self):
        mock_params = build_mock_fuzzer_params(npm_command="jstestfuzz")
        fuzzer_service = under_test.FuzzerGenTaskService()

        sub_task = fuzzer_service.build_fuzzer_sub_task(3, mock_params)

        self.assertEqual(sub_task.name, f"{mock_params.task_name}_3_{mock_params.variant}")
        self.assertEqual(len(sub_task.commands), 7)

    def test_sub_task_should_include_timeout_info(self):
        mock_params = build_mock_fuzzer_params()
        fuzzer_service = under_test.FuzzerGenTaskService()

        sub_task = fuzzer_service.build_fuzzer_sub_task(3, mock_params)

        cmd = sub_task.commands[0].as_dict()

        self.assertEqual(cmd["command"], "timeout.update")
        self.assertEqual(cmd["params"]["timeout_secs"], mock_params.timeout_secs)
