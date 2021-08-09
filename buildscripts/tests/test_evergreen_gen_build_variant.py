"""Unit tests for the generate_resmoke_suite script."""
import unittest

from mock import MagicMock

from buildscripts import evergreen_gen_build_variant as under_test
from buildscripts.ciconfig.evergreen import Variant, Task

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access
# pylint: disable=too-many-locals,too-many-lines,too-many-public-methods,no-value-for-parameter


def build_mock_build_variant(expansions=None, task_list=None):
    task_spec_list = [{"name": task.name} for task in task_list] if task_list else []
    config = {
        "tasks": task_spec_list,
    }
    if expansions:
        config["expansions"] = expansions

    if task_list is None:
        task_list = []
    task_map = {task.name: task for task in task_list}

    return Variant(config, task_map, {})


def build_mock_task(name, run_vars=None):
    config = {
        "name":
            name, "commands": [
                {"func": "do setup"},
                {
                    "func": "generate resmoke tasks",
                    "vars": run_vars if run_vars else {},
                },
            ]
    }
    return Task(config)


def build_mock_project_config(variant=None, task_defs=None):
    mock_project = MagicMock()
    if variant:
        mock_project.get_variant.return_value = variant

    if task_defs:
        mock_project.get_task.side_effect = task_defs

    return mock_project


def build_mock_expansions():
    mock_expansions = MagicMock()
    mock_expansions.config_location.return_value = "/path/to/config"
    return mock_expansions


def build_mock_evg_api(build_task_list):
    mock_evg_api = MagicMock()
    mock_evg_api.build_by_id.return_value.get_tasks.return_value = build_task_list
    return mock_evg_api


def build_mock_orchestrator(build_expansions=None, task_def_list=None, build_task_list=None):
    if build_expansions is None:
        build_expansions = {}
    if task_def_list is None:
        task_def_list = []
    if build_task_list is None:
        build_task_list = []

    mock_build_variant = build_mock_build_variant(build_expansions, task_def_list)
    mock_project = build_mock_project_config(mock_build_variant, task_def_list)
    mock_evg_expansions = build_mock_expansions()
    mock_evg_api = build_mock_evg_api(build_task_list)

    return under_test.GenerateBuildVariantOrchestrator(
        gen_task_validation=MagicMock(),
        gen_task_options=MagicMock(),
        evg_project_config=mock_project,
        evg_expansions=mock_evg_expansions,
        multiversion_util=MagicMock(),
        evg_api=mock_evg_api,
    )


class TestTranslateRunVar(unittest.TestCase):
    def test_normal_value_should_be_returned(self):
        run_var = "some value"
        mock_build_variant = build_mock_build_variant()
        self.assertEqual(run_var, under_test.translate_run_var(run_var, mock_build_variant))

    def test_expansion_should_be_returned_from_build_variant(self):
        run_var = "${my_expansion}"
        value = "my value"
        mock_build_variant = build_mock_build_variant(expansions={"my_expansion": value})
        self.assertEqual(value, under_test.translate_run_var(run_var, mock_build_variant))

    def test_expansion_not_found_should_return_none(self):
        run_var = "${my_expansion}"
        mock_build_variant = build_mock_build_variant(expansions={})
        self.assertIsNone(under_test.translate_run_var(run_var, mock_build_variant))

    def test_expansion_not_found_should_return_default(self):
        run_var = "${my_expansion|default}"
        mock_build_variant = build_mock_build_variant(expansions={})
        self.assertEqual("default", under_test.translate_run_var(run_var, mock_build_variant))

    def test_expansion_should_be_returned_from_build_variant_even_with_default(self):
        run_var = "${my_expansion|default}"
        value = "my value"
        mock_build_variant = build_mock_build_variant(expansions={"my_expansion": value})
        self.assertEqual(value, under_test.translate_run_var(run_var, mock_build_variant))


class TestTaskDefToSplitParams(unittest.TestCase):
    def test_params_should_be_generated(self):
        run_vars = {
            "resmoke_args": "run tests",
        }
        mock_task_def = build_mock_task("my_task", run_vars)
        mock_orchestrator = build_mock_orchestrator(task_def_list=[mock_task_def])

        split_param = mock_orchestrator.task_def_to_split_params(mock_task_def, "build_variant")

        self.assertEqual("build_variant", split_param.build_variant)
        self.assertEqual("my_task", split_param.task_name)
        self.assertEqual("my_task", split_param.suite_name)
        self.assertEqual("my_task", split_param.filename)

    def test_params_should_allow_suite_to_be_overridden(self):
        run_vars = {
            "resmoke_args": "run tests",
            "suite": "the suite",
        }
        mock_task_def = build_mock_task("my_task", run_vars)
        mock_orchestrator = build_mock_orchestrator(task_def_list=[mock_task_def])

        split_param = mock_orchestrator.task_def_to_split_params(mock_task_def, "build_variant")

        self.assertEqual("build_variant", split_param.build_variant)
        self.assertEqual("my_task", split_param.task_name)
        self.assertEqual("the suite", split_param.suite_name)
        self.assertEqual("the suite", split_param.filename)


class TestTaskDefToGenParams(unittest.TestCase):
    def test_params_should_be_generated(self):
        run_vars = {
            "resmoke_args": "run tests",
        }
        mock_task_def = build_mock_task("my_task", run_vars)
        mock_orchestrator = build_mock_orchestrator(task_def_list=[mock_task_def])

        gen_params = mock_orchestrator.task_def_to_gen_params(mock_task_def, "build_variant")

        self.assertIsNone(gen_params.require_multiversion)
        self.assertEqual("run tests", gen_params.resmoke_args)
        self.assertEqual(mock_orchestrator.evg_expansions.config_location.return_value,
                         gen_params.config_location)
        self.assertIsNone(gen_params.large_distro_name)
        self.assertFalse(gen_params.use_large_distro)

    def test_params_should_be_overwritable(self):
        run_vars = {
            "resmoke_args": "run tests",
            "use_large_distro": "true",
            "require_multiversion": True,
        }
        mock_task_def = build_mock_task("my_task", run_vars)
        build_expansions = {"large_distro_name": "my large distro"}
        mock_orchestrator = build_mock_orchestrator(build_expansions=build_expansions,
                                                    task_def_list=[mock_task_def])
        gen_params = mock_orchestrator.task_def_to_gen_params(mock_task_def, "build_variant")

        self.assertTrue(gen_params.require_multiversion)
        self.assertEqual("run tests", gen_params.resmoke_args)
        self.assertEqual(mock_orchestrator.evg_expansions.config_location.return_value,
                         gen_params.config_location)
        self.assertEqual("my large distro", gen_params.large_distro_name)
        self.assertTrue(gen_params.use_large_distro)


class TestTaskDefToFuzzerParams(unittest.TestCase):
    def test_params_should_be_generated(self):
        run_vars = {
            "name": "my_fuzzer",
            "num_files": "5",
            "num_tasks": "3",
        }
        mock_task_def = build_mock_task("my_task", run_vars)
        mock_orchestrator = build_mock_orchestrator(task_def_list=[mock_task_def])
        fuzzer_params = mock_orchestrator.task_def_to_fuzzer_params(mock_task_def, "build_variant")

        self.assertEqual("my_fuzzer", fuzzer_params.task_name)
        self.assertEqual(5, fuzzer_params.num_files)
        self.assertEqual(3, fuzzer_params.num_tasks)
        self.assertEqual("jstestfuzz", fuzzer_params.npm_command)
        self.assertEqual(mock_orchestrator.evg_expansions.config_location.return_value,
                         fuzzer_params.config_location)
        self.assertIsNone(fuzzer_params.large_distro_name)
        self.assertFalse(fuzzer_params.use_large_distro)

    def test_params_should_be_overwritable(self):
        run_vars = {
            "name": "my_fuzzer",
            "num_files": "${file_count|8}",
            "num_tasks": "3",
            "use_large_distro": "true",
            "npm_command": "aggfuzzer",
        }
        mock_task_def = build_mock_task("my_task", run_vars)
        build_expansions = {"large_distro_name": "my large distro"}
        mock_orchestrator = build_mock_orchestrator(build_expansions=build_expansions,
                                                    task_def_list=[mock_task_def])

        fuzzer_params = mock_orchestrator.task_def_to_fuzzer_params(mock_task_def, "build_variant")

        self.assertEqual("my_fuzzer", fuzzer_params.task_name)
        self.assertEqual(8, fuzzer_params.num_files)
        self.assertEqual(3, fuzzer_params.num_tasks)
        self.assertEqual("aggfuzzer", fuzzer_params.npm_command)
        self.assertEqual(mock_orchestrator.evg_expansions.config_location.return_value,
                         fuzzer_params.config_location)
        self.assertEqual("my large distro", fuzzer_params.large_distro_name)
        self.assertTrue(fuzzer_params.use_large_distro)


class TestGenerateBuildVariant(unittest.TestCase):
    def test_a_whole_build_variant(self):
        gen_run_vars = {
            "resmoke_args": "run tests",
        }
        mv_gen_run_vars = {
            "resmoke_args": "run tests",
            "suite": "multiversion suite",
            "implicit_multiversion": "True",
        }
        fuzz_run_vars = {
            "name": "my_fuzzer",
            "num_files": "5",
            "num_tasks": "3",
            "is_jstestfuzz": "True",
        }
        mv_fuzz_run_vars = {
            "name": "my_fuzzer",
            "num_files": "5",
            "num_tasks": "3",
            "is_jstestfuzz": "True",
            "suite": "aggfuzzer",
            "implicit_multiversion": "True",
        }
        mock_task_defs = [
            build_mock_task("my_gen_task", gen_run_vars),
            build_mock_task("my_fuzzer_task", fuzz_run_vars),
            build_mock_task("my_mv_fuzzer_task", mv_fuzz_run_vars),
            build_mock_task("my_mv_gen_task", mv_gen_run_vars),
        ]
        mock_orchestrator = build_mock_orchestrator(task_def_list=mock_task_defs)
        builder = MagicMock()

        builder = mock_orchestrator.generate_build_variant(builder, "build variant")

        self.assertEqual(builder.generate_suite.call_count, 1)
        self.assertEqual(builder.generate_fuzzer.call_count, 2)
        self.assertEqual(builder.add_multiversion_suite.call_count, 1)


class TestAdjustTaskPriority(unittest.TestCase):
    def test_task_is_updates(self):
        starting_priority = 42
        task_id = "task 314"
        mock_task = MagicMock(task_id=task_id, priority=starting_priority)
        mock_orchestrator = build_mock_orchestrator()

        mock_orchestrator.adjust_task_priority(mock_task)

        mock_orchestrator.evg_api.configure_task.assert_called_with(task_id,
                                                                    priority=starting_priority + 1)

    def test_task_should_only_reach_99(self):
        starting_priority = 99
        task_id = "task 314"
        mock_task = MagicMock(task_id=task_id, priority=starting_priority)
        mock_orchestrator = build_mock_orchestrator()

        mock_orchestrator.adjust_task_priority(mock_task)

        mock_orchestrator.evg_api.configure_task.assert_called_with(task_id,
                                                                    priority=starting_priority)


class TestAdjustGenTasksPriority(unittest.TestCase):
    def test_gen_tasks_in_task_list_are_adjusted(self):
        gen_tasks = {"task_3", "task_8", "task_13"}
        n_build_tasks = 25
        mock_task_list = [
            MagicMock(display_name=f"task_{i}", priority=0) for i in range(n_build_tasks)
        ]
        mock_orchestrator = build_mock_orchestrator(build_task_list=mock_task_list)

        n_tasks_adjusted = mock_orchestrator.adjust_gen_tasks_priority(gen_tasks)

        self.assertEqual(len(gen_tasks), n_tasks_adjusted)
