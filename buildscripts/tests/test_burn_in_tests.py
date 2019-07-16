"""Unit tests for buildscripts/burn_in_tests.py."""

from __future__ import absolute_import

import collections
import os
import sys
import subprocess
import unittest

from mock import Mock, mock_open, patch, MagicMock

import buildscripts.burn_in_tests as burn_in
import buildscripts.ciconfig.evergreen as evg

# pylint: disable=missing-docstring,protected-access,too-many-lines

BURN_IN = "buildscripts.burn_in_tests"
EVG_CI = "buildscripts.ciconfig.evergreen"
EVG_CLIENT = "buildscripts.client.evergreen"
GIT = "buildscripts.git"
RESMOKELIB = "buildscripts.resmokelib"

GENERATE_RESMOKE_TASKS_BASENAME = "this_is_a_gen_task"
GENERATE_RESMOKE_TASKS_NAME = GENERATE_RESMOKE_TASKS_BASENAME + "_gen"
GET_GENERATE_RESMOKE_TASKS_NAME = lambda _: GENERATE_RESMOKE_TASKS_NAME
GENERATE_RESMOKE_TASKS_COMMAND = {
    "func": "generate resmoke tasks",
    "vars": {"suite": "suite3", "resmoke_args": "--shellWriteMode=commands"}
}

GENERATE_RESMOKE_TASKS_COMMAND2 = {
    "func": "generate resmoke tasks", "vars": {"resmoke_args": "--shellWriteMode=commands"}
}

MULTIVERSION_PATH = "/data/multiversion"
GENERATE_RESMOKE_TASKS_MULTIVERSION_COMMAND = {
    "func": "generate resmoke tasks",
    "vars": {"resmoke_args": "--shellWriteMode=commands", "use_multiversion": MULTIVERSION_PATH}
}

MULTIVERSION_COMMAND = {"func": "do multiversion setup"}
RUN_TESTS_MULTIVERSION_COMMAND = {
    "func": "run tests",
    "vars": {"resmoke_args": "--shellWriteMode=commands", "task_path_suffix": MULTIVERSION_PATH}
}

NS = "buildscripts.burn_in_tests"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def tasks_mock(  #pylint: disable=too-many-arguments
        tasks, generate_resmoke_tasks_command=None, get_vars_task_name=None, run_tests_command=None,
        multiversion_path=None, multiversion_setup_command=None):
    task_list = Mock()
    task_list.tasks = []
    for idx, task in enumerate(tasks):
        task_list.tasks.append(Mock())
        task_list.tasks[idx].is_generate_resmoke_task = generate_resmoke_tasks_command is not None
        task_list.tasks[idx].is_run_tests_task = run_tests_command is not None
        task_list.tasks[idx].is_multiversion_task = multiversion_path is not None
        task_list.tasks[idx].generate_resmoke_tasks_command = generate_resmoke_tasks_command
        task_list.tasks[idx].run_tests_command = run_tests_command
        task_list.tasks[idx].get_vars_task_name = get_vars_task_name
        task_list.tasks[idx].name = task["name"]
        resmoke_args = task.get("combined_resmoke_args")
        task_list.tasks[idx].combined_resmoke_args = resmoke_args
        task_list.tasks[idx].resmoke_suite = evg.ResmokeArgs.get_arg(
            resmoke_args, "suites") if resmoke_args else None
        task_list.tasks[idx].multiversion_path = multiversion_path
        task_list.tasks[idx].multiversion_setup_command = multiversion_setup_command
        if task["name"].endswith("_gen"):
            task_list.tasks[idx].generated_task_name = task["name"][:-4]

    return task_list


VARIANTS = {
    "variantall":
        tasks_mock([{"name": "task1", "combined_resmoke_args": "--suites=suite1 var1arg1"},
                    {"name": "task2", "combined_resmoke_args": "--suites=suite1 var1arg2"},
                    {"name": "task3", "combined_resmoke_args": "--suites=suite1 var1arg3"}]),
    "variant1":
        tasks_mock([{"name": "task1", "combined_resmoke_args": "--suites=suite1 var1arg1"},
                    {"name": "task2"}]),
    "variant2":
        tasks_mock([{"name": "task2", "combined_resmoke_args": "var2arg1"},
                    {"name": "task3", "combined_resmoke_args": "--suites=suite3 var2arg3"}]),
    "variant3":
        tasks_mock([{"name": "task2", "combined_resmoke_args": "var3arg1"}]),
    "variant4":
        tasks_mock([]),
    "variant_multiversion":
        tasks_mock(
            [{"name": "multiversion_task", "combined_resmoke_args": "--suites=suite3 vararg"}],
            run_tests_command=RUN_TESTS_MULTIVERSION_COMMAND,
            multiversion_setup_command=RUN_TESTS_MULTIVERSION_COMMAND,
            multiversion_path=MULTIVERSION_PATH),
    "variant_generate_tasks":
        tasks_mock([{
            "name": GENERATE_RESMOKE_TASKS_NAME, "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_COMMAND,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME),
    "variant_generate_tasks_no_suite":
        tasks_mock([{
            "name": GENERATE_RESMOKE_TASKS_NAME, "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_COMMAND2,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME),
    "variant_generate_tasks_diff_names":
        tasks_mock([{
            "name": "gen_task_name_different_from_vars_task_name",
            "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_COMMAND,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME),
    "variant_generate_tasks_multiversion":
        tasks_mock([{
            "name": GENERATE_RESMOKE_TASKS_NAME, "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_MULTIVERSION_COMMAND,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME,
                   multiversion_path=MULTIVERSION_PATH),
}

EVERGREEN_CONF = Mock()
EVERGREEN_CONF.get_variant = VARIANTS.get
EVERGREEN_CONF.variant_names = VARIANTS.keys()


def _mock_parser():
    parser = Mock()
    parser.error = Mock()
    return parser


class TestValidateOptions(unittest.TestCase):
    @staticmethod
    def _mock_options():
        options = Mock()
        options.repeat_tests_num = None
        options.repeat_tests_max = None
        options.repeat_tests_min = None
        options.repeat_tests_secs = None
        options.buildvariant = None
        options.run_buildvariant = None
        options.test_list_file = None
        return options

    def test_validate_options_listfile_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.test_list_file = "list_file.json"
        options.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_nolistfile_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_listfile_nobuildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.test_list_file = "list_file.json"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_no_listfile_no_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_run_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        options.run_buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_bad_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "badvariant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_bad_run_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.run_buildvariant = "badvariant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_max_no_tests_secs(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_max = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_min_no_tests_secs(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_min = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_min_gt_tests_max(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_min = 3
        options.repeat_tests_max = 2
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_secs(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        options.repeat_tests_min = 2
        options.repeat_tests_max = 1000
        options.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_tests_secs_and_tests_num(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        options.repeat_tests_num = 1
        options.repeat_tests_min = 1
        options.repeat_tests_max = 3
        options.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_secs_no_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_min = 1
        options.repeat_tests_max = 3
        options.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()


class TestGetResmokeRepeatOptions(unittest.TestCase):
    @staticmethod
    def _options_mock():
        options = Mock()
        options.repeat_tests_secs = None
        options.repeat_tests_min = None
        options.repeat_tests_max = None
        options.repeat_tests_num = None
        return options

    def test_get_resmoke_repeat_options_default(self):
        options = self._options_mock()
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(burn_in.REPEAT_SUITES))

    def test_get_resmoke_repeat_options_num(self):
        options = self._options_mock()
        options.repeat_tests_num = 5
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(options.repeat_tests_num))

    def test_get_resmoke_repeat_options_secs(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatTestsSecs={}".format(options.repeat_tests_secs))

    def test_get_resmoke_repeat_options_secs_min(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        options.repeat_tests_min = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertIn("--repeatTestsSecs={}".format(options.repeat_tests_secs), repeat_options)
        self.assertIn("--repeatTestsMin={}".format(options.repeat_tests_min), repeat_options)
        self.assertNotIn("--repeatTestsMax", repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_secs_max(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        options.repeat_tests_max = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertIn("--repeatTestsSecs={}".format(options.repeat_tests_secs), repeat_options)
        self.assertIn("--repeatTestsMax={}".format(options.repeat_tests_max), repeat_options)
        self.assertNotIn("--repeatTestsMin", repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_secs_min_max(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        options.repeat_tests_min = 2
        options.repeat_tests_max = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertIn("--repeatTestsSecs={}".format(options.repeat_tests_secs), repeat_options)
        self.assertIn("--repeatTestsMin={}".format(options.repeat_tests_min), repeat_options)
        self.assertIn("--repeatTestsMax={}".format(options.repeat_tests_max), repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_min(self):
        options = self._options_mock()
        options.repeat_tests_min = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(burn_in.REPEAT_SUITES))

    def test_get_resmoke_repeat_options_max(self):
        options = self._options_mock()
        options.repeat_tests_max = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(burn_in.REPEAT_SUITES))


class TestCheckVariant(unittest.TestCase):
    @staticmethod
    def test_check_variant():
        mock_parser = _mock_parser()
        buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.check_variant(buildvariant, mock_parser)
            mock_parser.error.assert_not_called()

    @staticmethod
    def test_check_variant_badvariant():
        mock_parser = _mock_parser()
        buildvariant = "badvariant"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.check_variant(buildvariant, mock_parser)
            mock_parser.error.assert_called()


class TestGetRunBuildvariant(unittest.TestCase):
    def test__get_run_buildvariant_rb(self):
        run_buildvariant = "variant1"
        buildvariant = "variant2"
        options = Mock()
        options.run_buildvariant = run_buildvariant
        options.buildvariant = buildvariant
        self.assertEqual(run_buildvariant, burn_in._get_run_buildvariant(options))

    def test__get_run_buildvariant_bv(self):
        buildvariant = "variant2"
        options = Mock()
        options.run_buildvariant = None
        options.buildvariant = buildvariant
        self.assertEqual(buildvariant, burn_in._get_run_buildvariant(options))


class TestGetTaskName(unittest.TestCase):
    def test__get_task_name(self):
        name = "mytask"
        task = Mock()
        task.is_generate_resmoke_task = False
        task.name = name
        self.assertEqual(name, burn_in._get_task_name(task))

    def test__get_task_name_generate_resmoke_task(self):
        task_name = "mytask"
        task = Mock(is_generate_resmoke_task=True, generated_task_name=task_name)
        self.assertEqual(task_name, burn_in._get_task_name(task))


class TestSetResmokeArgs(unittest.TestCase):
    def test__set_resmoke_args(self):
        resmoke_args = "--suites=suite1 test1.js"
        task = Mock()
        task.combined_resmoke_args = resmoke_args
        task.is_generate_resmoke_task = False
        self.assertEqual(resmoke_args, burn_in._set_resmoke_args(task))

    def test__set_resmoke_args_gen_resmoke_task(self):
        resmoke_args = "--suites=suite1 test1.js"
        new_suite = "suite2"
        new_resmoke_args = "--suites={} test1.js".format(new_suite)
        task = Mock()
        task.combined_resmoke_args = resmoke_args
        task.is_generate_resmoke_task = True
        task.get_vars_suite_name = lambda cmd_vars: cmd_vars["suite"]
        task.generate_resmoke_tasks_command = {"vars": {"suite": new_suite}}
        self.assertEqual(new_resmoke_args, burn_in._set_resmoke_args(task))

    def test__set_resmoke_args_gen_resmoke_task_no_suite(self):
        suite = "suite1"
        resmoke_args = "--suites={} test1.js".format(suite)
        task = Mock()
        task.combined_resmoke_args = resmoke_args
        task.is_generate_resmoke_task = True
        task.get_vars_suite_name = lambda cmd_vars: cmd_vars["task"]
        task.generate_resmoke_tasks_command = {"vars": {"task": suite}}
        self.assertEqual(resmoke_args, burn_in._set_resmoke_args(task))


class TestSetResmokeCmd(unittest.TestCase):
    def test__set_resmoke_cmd_no_opts_no_args(self):
        with patch(BURN_IN + ".get_resmoke_repeat_options", return_value=""):
            self.assertListEqual([sys.executable, "buildscripts/resmoke.py"],
                                 burn_in._set_resmoke_cmd(None, None))

    def test__set_resmoke_cmd_no_opts(self):
        args = ["arg1", "arg2"]
        with patch(BURN_IN + ".get_resmoke_repeat_options", return_value=""):
            self.assertListEqual(args, burn_in._set_resmoke_cmd(None, args))

    def test__set_resmoke_cmd(self):
        opts = "myopt1 myopt2"
        args = ["arg1", "arg2"]
        new_cmd = args + opts.split()
        with patch(BURN_IN + ".get_resmoke_repeat_options", return_value=opts):
            self.assertListEqual(new_cmd, burn_in._set_resmoke_cmd(opts, args))


class TestSubTaskName(unittest.TestCase):
    def test__sub_task_name(self):
        options = MagicMock(buildvariant="myvar", run_buildvariant=None)
        task = "mytask"
        task_num = 0
        self.assertEqual("burn_in:myvar_mytask_0", burn_in._sub_task_name(options, task, task_num))

    def test__sub_task_name_with_run_bv(self):
        options = MagicMock(buildvariant="myvar", run_buildvariant="run_var")
        task = "mytask"
        task_num = 0
        self.assertEqual("burn_in:run_var_mytask_0", burn_in._sub_task_name(
            options, task, task_num))


TESTS_BY_TASK = {
    "task1": {
        "resmoke_args": "--suites=suite1",
        "tests": ["jstests/test1.js", "jstests/test2.js"]},
    "task2": {
        "resmoke_args": "--suites=suite1",
        "tests": ["jstests/test1.js", "jstests/test3.js"]},
    "task3": {
        "resmoke_args": "--suites=suite3",
        "tests": ["jstests/test4.js", "jstests/test5.js"]},
    "task4": {
        "resmoke_args": "--suites=suite4", "tests": []},
    "taskmulti": {
        "resmoke_args": "--suites=suite4",
        "tests": ["jstests/multi1.js"],
        "use_multiversion": "/data/multi"},
} # yapf: disable


class TestCreateGenerateTasksFile(unittest.TestCase):
    @staticmethod
    def _options_mock():
        options = Mock()
        options.buildvariant = None
        options.run_buildvariant = None
        options.distro = None
        return options

    @staticmethod
    def _get_tests(tests_by_task):
        tests = []
        for task in tests_by_task:
            tests.extend(tests_by_task[task]["tests"])
        return tests

    def test_create_generate_tasks_file_tasks(self):
        options = self._options_mock()
        options.buildvariant = "myvariant"
        tests_by_task = TESTS_BY_TASK
        test_tasks = self._get_tests(tests_by_task)
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            evg_tasks = evg_config["tasks"]
            self.assertEqual(len(evg_tasks), len(test_tasks))
            # Check task1 - test1.js
            task = evg_tasks[0]
            self.assertEqual(task["name"], "burn_in:myvariant_task1_0")
            self.assertEqual(len(task["depends_on"]), 1)
            self.assertEqual(task["depends_on"][0]["name"], "compile")
            self.assertEqual(len(task["commands"]), 2)
            self.assertEqual(task["commands"][0]["func"], "do setup")
            self.assertEqual(task["commands"][1]["func"], "run tests")
            resmoke_args = task["commands"][1]["vars"]["resmoke_args"]
            self.assertIn("--suites=suite1", resmoke_args)
            self.assertIn("jstests/test1.js", resmoke_args)
            # Check task1 - test2.js
            task = evg_tasks[1]
            self.assertEqual(task["name"], "burn_in:myvariant_task1_1")
            self.assertEqual(len(task["depends_on"]), 1)
            self.assertEqual(task["depends_on"][0]["name"], "compile")
            self.assertEqual(len(task["commands"]), 2)
            self.assertEqual(task["commands"][0]["func"], "do setup")
            self.assertEqual(task["commands"][1]["func"], "run tests")
            resmoke_args = task["commands"][1]["vars"]["resmoke_args"]
            self.assertIn("--suites=suite1", resmoke_args)
            self.assertIn("jstests/test2.js", resmoke_args)
            # task[2] - task[5] are similar to task[0] & task[1]
            # Check taskmulti - multi1.js
            taskmulti = evg_tasks[6]
            self.assertEqual(taskmulti["name"], "burn_in:myvariant_taskmulti_0")
            self.assertEqual(len(taskmulti["depends_on"]), 1)
            self.assertEqual(taskmulti["depends_on"][0]["name"], "compile")
            self.assertEqual(len(taskmulti["commands"]), 3)
            self.assertEqual(taskmulti["commands"][0]["func"], "do setup")
            self.assertEqual(taskmulti["commands"][1]["func"], "do multiversion setup")
            self.assertEqual(taskmulti["commands"][2]["func"], "run tests")
            resmoke_args = taskmulti["commands"][2]["vars"]["resmoke_args"]
            self.assertIn("--suites=suite4", resmoke_args)
            self.assertIn("jstests/multi1.js", resmoke_args)
            self.assertEqual(taskmulti["commands"][2]["vars"]["task_path_suffix"], "/data/multi")

    def test_create_generate_tasks_file_variants(self):
        options = self._options_mock()
        options.buildvariant = "myvariant"
        tests_by_task = TESTS_BY_TASK
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            self.assertEqual(evg_config["buildvariants"][0]["name"], "myvariant")
            self.assertEqual(len(evg_config["buildvariants"][0]["tasks"]), 7)
            self.assertEqual(len(evg_config["buildvariants"][0]["display_tasks"]), 1)
            display_task = evg_config["buildvariants"][0]["display_tasks"][0]
            self.assertEqual(display_task["name"], burn_in.BURN_IN_TESTS_TASK)
            execution_tasks = display_task["execution_tasks"]
            self.assertEqual(len(execution_tasks), 8)
            self.assertEqual(execution_tasks[0], burn_in.BURN_IN_TESTS_GEN_TASK)
            self.assertEqual(execution_tasks[1], "burn_in:myvariant_task1_0")
            self.assertEqual(execution_tasks[2], "burn_in:myvariant_task1_1")
            self.assertEqual(execution_tasks[3], "burn_in:myvariant_task2_0")
            self.assertEqual(execution_tasks[4], "burn_in:myvariant_task2_1")
            self.assertEqual(execution_tasks[5], "burn_in:myvariant_task3_0")
            self.assertEqual(execution_tasks[6], "burn_in:myvariant_task3_1")
            self.assertEqual(execution_tasks[7], "burn_in:myvariant_taskmulti_0")

    def test_create_generate_tasks_file_run_variants(self):
        options = self._options_mock()
        options.buildvariant = "myvariant"
        options.run_buildvariant = "run_variant"
        tests_by_task = TESTS_BY_TASK
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            self.assertEqual(evg_config["buildvariants"][0]["name"], "run_variant")
            self.assertEqual(len(evg_config["buildvariants"][0]["tasks"]), 7)
            self.assertEqual(len(evg_config["buildvariants"][0]["display_tasks"]), 1)
            display_task = evg_config["buildvariants"][0]["display_tasks"][0]
            self.assertEqual(display_task["name"], burn_in.BURN_IN_TESTS_TASK)
            execution_tasks = display_task["execution_tasks"]
            self.assertEqual(len(execution_tasks), 8)
            self.assertEqual(execution_tasks[0], burn_in.BURN_IN_TESTS_GEN_TASK)
            self.assertEqual(execution_tasks[1], "burn_in:run_variant_task1_0")
            self.assertEqual(execution_tasks[2], "burn_in:run_variant_task1_1")
            self.assertEqual(execution_tasks[3], "burn_in:run_variant_task2_0")
            self.assertEqual(execution_tasks[4], "burn_in:run_variant_task2_1")
            self.assertEqual(execution_tasks[5], "burn_in:run_variant_task3_0")
            self.assertEqual(execution_tasks[6], "burn_in:run_variant_task3_1")
            self.assertEqual(execution_tasks[7], "burn_in:run_variant_taskmulti_0")

    def test_create_generate_tasks_file_distro(self):
        options = self._options_mock()
        options.buildvariant = "myvariant"
        options.distro = "mydistro"
        tests_by_task = TESTS_BY_TASK
        test_tasks = self._get_tests(tests_by_task)
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config["tasks"]), len(test_tasks))
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            for variant in evg_config["buildvariants"]:
                for task in variant.get("tasks", []):
                    self.assertEqual(len(task["distros"]), 1)
                    self.assertEqual(task["distros"][0], options.distro)

    def test_create_generate_tasks_file_no_tasks(self):
        variant = "myvariant"
        options = self._options_mock()
        options.buildvariant = variant
        tests_by_task = {}
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config), 1)
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            self.assertEqual(evg_config["buildvariants"][0]["name"], variant)
            display_tasks = evg_config["buildvariants"][0]["display_tasks"]
            self.assertEqual(len(display_tasks), 1)
            self.assertEqual(display_tasks[0]["name"], burn_in.BURN_IN_TESTS_TASK)
            execution_tasks = display_tasks[0]["execution_tasks"]
            self.assertEqual(len(execution_tasks), 1)
            self.assertEqual(execution_tasks[0], burn_in.BURN_IN_TESTS_GEN_TASK)


class UpdateReportDataTests(unittest.TestCase):
    def test_update_report_data_nofile(self):
        data = {}
        task = ""
        pathname = "file_exists"
        with patch("os.path.isfile", return_value=False) as mock_isfile,\
             patch("json.load", return_value=data) as mock_json:
            burn_in._update_report_data(data, pathname, task)
            self.assertEqual(mock_isfile.call_count, 1)
            self.assertEqual(mock_json.call_count, 0)

    def test_update_report_data(self):
        task1 = "task1"
        task2 = "task2"
        data = {
            "failures": 1,
            "results": [
                {"test_file": "test1:" + task1},
                {"test_file": "test2:" + task1}]
        } # yapf: disable
        new_data = {
            "failures": 1,
            "results": [
                {"test_file": "test3"},
                {"test_file": "test4"}]
        } # yapf: disable

        pathname = "file_exists"
        with patch("os.path.isfile", return_value=True),\
             patch("builtins.open", mock_open()),\
             patch("json.load", return_value=new_data):
            burn_in._update_report_data(data, pathname, task2)
            self.assertEqual(len(data["results"]), 4)
            self.assertEqual(data["failures"], 2)
            self.assertIn({"test_file": "test1:" + task1}, data["results"])
            self.assertIn({"test_file": "test3:" + task2}, data["results"])


class RunTests(unittest.TestCase):
    class SysExit(Exception):
        pass

    def _test_run_tests(self, no_exec, tests_by_task, resmoke_cmd):
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch(BURN_IN + "._update_report_data", return_value=None),\
             patch(BURN_IN + "._write_json_file", return_value=None):
            burn_in.run_tests(no_exec, tests_by_task, resmoke_cmd, None)
            self.assertEqual(mock_subproc.call_count, len(tests_by_task.keys()))
            for idx, task in enumerate(sorted(tests_by_task)):
                for task_test in tests_by_task[task].get("tests", []):
                    self.assertIn(task_test, mock_subproc.call_args_list[idx][0][0])

    def test_run_tests_noexec(self):
        no_exec = True
        resmoke_cmd = None
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch(BURN_IN + "._write_json_file", return_value=None) as mock_write_json:
            burn_in.run_tests(no_exec, TESTS_BY_TASK, resmoke_cmd, None)
            self.assertEqual(mock_subproc.call_count, 0)
            self.assertEqual(mock_write_json.call_count, 0)

    def test_run_tests_notests(self):
        no_exec = False
        tests_by_task = {}
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        self._test_run_tests(no_exec, tests_by_task, resmoke_cmd)

    def test_run_tests_tests(self):
        no_exec = False
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        self._test_run_tests(no_exec, TESTS_BY_TASK, resmoke_cmd)

    def test_run_tests_tests_resmoke_failure(self):
        no_exec = False
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        error_code = -1
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch("sys.exit", return_value=error_code) as mock_exit,\
             patch(BURN_IN + "._update_report_data", return_value=None),\
             patch(BURN_IN + "._write_json_file", return_value=None):
            mock_subproc.side_effect = subprocess.CalledProcessError(error_code, "err1")
            mock_exit.side_effect = self.SysExit(error_code)
            with self.assertRaises(self.SysExit):
                burn_in.run_tests(no_exec, TESTS_BY_TASK, resmoke_cmd, None)


MEMBERS_MAP = {
    "test1.js": ["suite1", "suite2"], "test2.js": ["suite1", "suite3"], "test3.js": [],
    "test4.js": ["suite1", "suite2", "suite3"], "test5.js": ["suite2"]
}

SUITE1 = Mock()
SUITE1.tests = ["test1.js", "test2.js", "test4.js"]
SUITE2 = Mock()
SUITE2.tests = ["test1.js"]
SUITE3 = Mock()
SUITE3.tests = ["test2.js", "test4.js"]


def _create_executor_list(suites, exclude_suites):
    with patch(RESMOKELIB + ".suitesconfig.create_test_membership_map", return_value=MEMBERS_MAP):
        return burn_in.create_executor_list(suites, exclude_suites)


class CreateExecutorList(unittest.TestCase):
    def test_create_executor_list_no_excludes(self):
        suites = [SUITE1, SUITE2]
        exclude_suites = []
        executor_list = _create_executor_list(suites, exclude_suites)
        self.assertEqual(executor_list["suite1"], SUITE1.tests)
        self.assertEqual(executor_list["suite2"], ["test1.js", "test4.js"])
        self.assertEqual(executor_list["suite3"], ["test2.js", "test4.js"])

    def test_create_executor_list_excludes(self):
        suites = [SUITE1, SUITE2]
        exclude_suites = ["suite3"]
        executor_list = _create_executor_list(suites, exclude_suites)
        self.assertEqual(executor_list["suite1"], SUITE1.tests)
        self.assertEqual(executor_list["suite2"], ["test1.js", "test4.js"])
        self.assertEqual(executor_list["suite3"], [])

    def test_create_executor_list_nosuites(self):
        executor_list = _create_executor_list([], [])
        self.assertEqual(executor_list, collections.defaultdict(list))

    @patch(RESMOKELIB + ".testing.suite.Suite")
    @patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_create_executor_list_runs_core_suite(self, mock_get_named_suites, mock_suite_class):
        mock_get_named_suites.return_value = ["core"]

        burn_in.create_executor_list([], [])
        self.assertEqual(mock_suite_class.call_count, 1)

    @patch(RESMOKELIB + ".testing.suite.Suite")
    @patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_create_executor_list_ignores_dbtest_suite(self, mock_get_named_suites,
                                                       mock_suite_class):
        mock_get_named_suites.return_value = ["dbtest"]

        burn_in.create_executor_list([], [])
        self.assertEqual(mock_suite_class.call_count, 0)


class CreateTaskList(unittest.TestCase):
    def test_create_task_list(self):
        variant = "variantall"
        suites = [SUITE1, SUITE2, SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variantall"].tasks))
        self.assertIn("task1", task_list)
        self.assertEqual(task_list["task1"]["resmoke_args"], "--suites=suite1 var1arg1")
        self.assertEqual(task_list["task1"]["tests"], SUITE1.tests)
        self.assertIsNone(task_list["task1"]["use_multiversion"])
        self.assertIn("task2", task_list)
        self.assertEqual(task_list["task2"]["resmoke_args"], "--suites=suite1 var1arg2")
        self.assertEqual(task_list["task2"]["tests"], SUITE1.tests)
        self.assertIsNone(task_list["task2"]["use_multiversion"])
        self.assertIn("task3", task_list)
        self.assertEqual(task_list["task3"]["resmoke_args"], "--suites=suite1 var1arg3")
        self.assertEqual(task_list["task3"]["tests"], SUITE1.tests)
        self.assertIsNone(task_list["task3"]["use_multiversion"])

    def test_create_task_list_multiversion(self):
        variant = "variant_multiversion"
        suites = [SUITE1, SUITE2, SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_multiversion"].tasks))
        self.assertEqual(task_list["multiversion_task"]["use_multiversion"], MULTIVERSION_PATH)

    def test_create_task_list_gen_tasks(self):
        variant = "variant_generate_tasks"
        suites = [SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_generate_tasks"].tasks))
        self.assertIn(GENERATE_RESMOKE_TASKS_BASENAME, task_list)
        self.assertEqual(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["tests"], SUITE3.tests)
        self.assertIsNone(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["use_multiversion"])

    def test_create_task_list_gen_tasks_multiversion(self):
        variant = "variant_generate_tasks_multiversion"
        suites = [SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_generate_tasks_multiversion"].tasks))
        self.assertEqual(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["use_multiversion"],
                         MULTIVERSION_PATH)

    def test_create_task_list_gen_tasks_no_suite(self):
        variant = "variant_generate_tasks_no_suite"
        suites = [SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_generate_tasks_no_suite"].tasks))
        self.assertIn(GENERATE_RESMOKE_TASKS_BASENAME, task_list)
        self.assertEqual(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["tests"], SUITE3.tests)

    def test_create_task_list_no_excludes(self):
        variant = "variant1"
        suites = [SUITE1, SUITE2]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), 1)
        self.assertIn("task1", task_list)
        self.assertEqual(task_list["task1"]["resmoke_args"], "--suites=suite1 var1arg1")
        self.assertEqual(task_list["task1"]["tests"], SUITE1.tests)
        self.assertNotIn("task2", task_list)
        self.assertNotIn("task3", task_list)

    def test_create_task_list_with_excludes(self):
        variant = "variant2"
        suites = [SUITE1, SUITE2, SUITE3]
        suite_list = _create_executor_list(suites, [])
        exclude_suites = ["suite2"]
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), 1)
        self.assertIn("task3", task_list)
        self.assertEqual(task_list["task3"]["resmoke_args"], "--suites=suite3 var2arg3")
        self.assertEqual(task_list["task3"]["tests"], SUITE3.tests)
        self.assertNotIn("task1", task_list)
        self.assertNotIn("task2", task_list)

    def test_create_task_list_no_suites(self):
        variant = "variant2"
        suite_list = {}
        exclude_suites = ["suite2"]
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), 0)
        self.assertEqual(task_list, {})

    def test_create_task_list_novariant(self):
        class BadVariant(Exception):
            pass

        def _raise_bad_variant(code=0):
            raise BadVariant("Bad variant {}".format(code))

        variant = "novariant"
        suites = [SUITE1, SUITE2, SUITE3]
        suite_list = _create_executor_list(suites, [])
        with patch("sys.exit", _raise_bad_variant):
            with self.assertRaises(BadVariant):
                burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, [])


class TestFindChangedTests(unittest.TestCase):
    @patch(ns("find_changed_files"))
    def test_nothing_found(self, changed_files_mock):
        repo_mock = MagicMock()
        changed_files_mock.return_value = set()

        self.assertEqual(0, len(burn_in.find_changed_tests(repo_mock)))

    @patch(ns("find_changed_files"))
    @patch(ns("os.path.isfile"))
    def test_non_js_files_filtered(self, is_file_mock, changed_files_mock):
        repo_mock = MagicMock()
        file_list = [
            os.path.join("jstests", "test1.js"),
            os.path.join("jstests", "test1.cpp"),
            os.path.join("jstests", "test2.js"),
        ]
        changed_files_mock.return_value = set(file_list)
        is_file_mock.return_value = True

        found_tests = burn_in.find_changed_tests(repo_mock)

        self.assertIn(file_list[0], found_tests)
        self.assertIn(file_list[2], found_tests)
        self.assertNotIn(file_list[1], found_tests)

    @patch(ns("find_changed_files"))
    @patch(ns("os.path.isfile"))
    def test_missing_files_filtered(self, is_file_mock, changed_files_mock):
        repo_mock = MagicMock()
        file_list = [
            os.path.join("jstests", "test1.js"),
            os.path.join("jstests", "test2.js"),
            os.path.join("jstests", "test3.js"),
        ]
        changed_files_mock.return_value = set(file_list)
        is_file_mock.return_value = False

        found_tests = burn_in.find_changed_tests(repo_mock)

        self.assertEqual(0, len(found_tests))

    @patch(ns("find_changed_files"))
    @patch(ns("os.path.isfile"))
    def test_non_jstests_files_filtered(self, is_file_mock, changed_files_mock):
        repo_mock = MagicMock()
        file_list = [
            os.path.join("jstests", "test1.js"),
            os.path.join("other", "test2.js"),
            os.path.join("jstests", "test3.js"),
        ]
        changed_files_mock.return_value = set(file_list)
        is_file_mock.return_value = True

        found_tests = burn_in.find_changed_tests(repo_mock)

        self.assertIn(file_list[0], found_tests)
        self.assertIn(file_list[2], found_tests)
        self.assertNotIn(file_list[1], found_tests)
        self.assertEqual(2, len(found_tests))
