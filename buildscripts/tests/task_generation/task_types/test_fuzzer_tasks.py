"""Unit tests for fuzzer_tasks.py"""

import unittest

import buildscripts.task_generation.task_types.fuzzer_tasks as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


def build_mock_fuzzer_params(multi_version=None, jstestfuzz_vars="vars for jstestfuzz",
                             version_config=None, is_sharded=None):
    return under_test.FuzzerGenTaskParams(
        task_name="task name",
        variant="build variant",
        suite="resmoke suite",
        num_files=10,
        num_tasks=5,
        resmoke_args="args for resmoke",
        npm_command="jstestfuzz",
        jstestfuzz_vars=jstestfuzz_vars,
        continue_on_failure=True,
        resmoke_jobs_max=5,
        should_shuffle=True,
        timeout_secs=100,
        require_multiversion=multi_version,
        use_large_distro=None,
        add_to_display_task=True,
        large_distro_name="large distro",
        config_location="config_location",
        version_config=version_config,
        is_sharded=is_sharded,
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

        self.assertEqual(params.task_name, params.get_task_name(""))

    def test_get_task_name_with_version(self):
        params = build_mock_fuzzer_params()
        expected_name = f"{params.suite}_multiversion_my_version"

        self.assertEqual(expected_name, params.get_task_name("my_version"))

    def test_get_non_sharded_resmoke_args(self):
        params = build_mock_fuzzer_params()

        self.assertEqual(params.resmoke_args, params.get_resmoke_args())

    def test_get_sharded_resmoke_args(self):
        params = build_mock_fuzzer_params(is_sharded=True)
        expected_args = f"{params.resmoke_args} --mixedBinVersions=None --numShards=2 --numReplSetNodes=2"

        self.assertEqual(expected_args, params.get_resmoke_args().strip())


class TestGenerateTasks(unittest.TestCase):
    def test_fuzzer_tasks_are_generated(self):
        mock_params = build_mock_fuzzer_params()
        fuzzer_service = under_test.FuzzerGenTaskService()

        fuzzer_task = fuzzer_service.generate_tasks(mock_params)

        self.assertEqual(fuzzer_task.task_name, mock_params.task_name)
        self.assertEqual(len(fuzzer_task.sub_tasks), mock_params.num_tasks)

    def test_fuzzers_for_multiversion_are_generated(self):
        version_list = ["version0", "version1"]
        mock_params = build_mock_fuzzer_params(version_config=version_list)
        fuzzer_service = under_test.FuzzerGenTaskService()

        fuzzer_task = fuzzer_service.generate_tasks(mock_params)

        self.assertEqual(fuzzer_task.task_name, mock_params.task_name)
        self.assertEqual(len(fuzzer_task.sub_tasks), mock_params.num_tasks * len(version_list))


class TestBuildFuzzerSubTask(unittest.TestCase):
    def test_sub_task_should_be_built_correct(self):
        mock_params = build_mock_fuzzer_params()
        fuzzer_service = under_test.FuzzerGenTaskService()

        sub_task = fuzzer_service.build_fuzzer_sub_task(3, mock_params, "")

        self.assertEqual(sub_task.name, f"{mock_params.task_name}_3_{mock_params.variant}")
        self.assertEqual(len(sub_task.commands), 5)

    def test_sub_task_multi_version_tasks_should_be_built_correct(self):
        mock_params = build_mock_fuzzer_params(multi_version="multiversion value")
        fuzzer_service = under_test.FuzzerGenTaskService()

        sub_task = fuzzer_service.build_fuzzer_sub_task(3, mock_params, "")

        self.assertEqual(sub_task.name, f"{mock_params.task_name}_3_{mock_params.variant}")
        self.assertEqual(len(sub_task.commands), 7)

    def test_sub_task_should_include_timeout_info(self):
        mock_params = build_mock_fuzzer_params(multi_version="multiversion value")
        fuzzer_service = under_test.FuzzerGenTaskService()

        sub_task = fuzzer_service.build_fuzzer_sub_task(3, mock_params, "")

        cmd = sub_task.commands[0].as_dict()

        self.assertEqual(cmd["command"], "timeout.update")
        self.assertEqual(cmd["params"]["timeout_secs"], mock_params.timeout_secs)
