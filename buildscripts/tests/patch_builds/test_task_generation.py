"""Unittests for buildscripts.patch_builds.task_generation.py"""
import unittest

from shrub.config import Configuration

import buildscripts.patch_builds.task_generation as under_test

# pylint: disable=missing-docstring,protected-access,too-many-lines,no-self-use


class TestResmokeCommand(unittest.TestCase):
    def test_basic_command(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.default_timeout()

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info)

        # 2 expected command = 1 for setup + 1 for running tests.
        self.assertEqual(2, len(commands))

    def test_with_multiversion(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.default_timeout()

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info,
                                               use_multiversion="multiversion")

        # 3 expected command = 1 for setup + 1 for running tests + 1 for multiversion setup.
        self.assertEqual(3, len(commands))

    def test_with_timeout(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.overridden(timeout=5)

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info)

        # 3 expected command = 1 for setup + 1 for running tests + 1 for timeout.
        self.assertEqual(3, len(commands))

    def test_with_everything(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.overridden(timeout=5)

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info,
                                               use_multiversion="multiversion")

        # 4 expected command = 1 for setup + 1 for running tests + 1 for multiversion setup +
        # 1 for timeout.
        self.assertEqual(4, len(commands))


class TestTimeoutInfo(unittest.TestCase):
    def test_default_timeout(self):
        timeout_info = under_test.TimeoutInfo.default_timeout()

        self.assertIsNone(timeout_info.cmd)

    def test_timeout_only_set(self):
        timeout = 5
        timeout_info = under_test.TimeoutInfo.overridden(timeout=timeout)

        cmd = timeout_info.cmd.to_map()

        self.assertEqual("timeout.update", cmd["command"])
        self.assertEqual(timeout, cmd["params"]["timeout_secs"])
        self.assertNotIn("exec_timeout_secs", cmd["params"])

    def test_exec_timeout_only_set(self):
        exec_timeout = 5
        timeout_info = under_test.TimeoutInfo.overridden(exec_timeout=exec_timeout)

        cmd = timeout_info.cmd.to_map()

        self.assertEqual("timeout.update", cmd["command"])
        self.assertEqual(exec_timeout, cmd["params"]["exec_timeout_secs"])
        self.assertNotIn("timeout_secs", cmd["params"])

    def test_both_timeouts_set(self):
        timeout = 3
        exec_timeout = 5
        timeout_info = under_test.TimeoutInfo.overridden(exec_timeout=exec_timeout, timeout=timeout)

        cmd = timeout_info.cmd.to_map()

        self.assertEqual("timeout.update", cmd["command"])
        self.assertEqual(exec_timeout, cmd["params"]["exec_timeout_secs"])
        self.assertEqual(timeout, cmd["params"]["timeout_secs"])

    def test_override_with_no_values(self):
        with self.assertRaises(ValueError):
            under_test.TimeoutInfo.overridden()


class TestTaskList(unittest.TestCase):
    def test_adding_a_task(self):
        config = Configuration()
        task_list = under_test.TaskList(config)

        func = "test"
        task = "task 1"
        variant = "variant 1"
        task_list.add_task(task, [under_test._cmd_by_name(func)])
        task_list.add_to_variant(variant)

        cfg_dict = config.to_map()

        cmd_dict = cfg_dict["tasks"][0]
        self.assertEqual(task, cmd_dict["name"])
        self.assertEqual(func, cmd_dict["commands"][0]["func"])

        self.assertEqual(task, cfg_dict["buildvariants"][0]["tasks"][0]["name"])

    def test_adding_a_task_with_distro(self):
        config = Configuration()
        task_list = under_test.TaskList(config)

        func = "test"
        task = "task 1"
        variant = "variant 1"
        distro = "distro 1"
        task_list.add_task(task, [under_test._cmd_by_name(func)], distro=distro)
        task_list.add_to_variant(variant)

        cfg_dict = config.to_map()

        cmd_dict = cfg_dict["tasks"][0]
        self.assertEqual(task, cmd_dict["name"])
        self.assertEqual(func, cmd_dict["commands"][0]["func"])

        self.assertEqual(task, cfg_dict["buildvariants"][0]["tasks"][0]["name"])
        self.assertIn(distro, cfg_dict["buildvariants"][0]["tasks"][0]["distros"])

    def test_adding_a_task_with_dependecies(self):
        config = Configuration()
        task_list = under_test.TaskList(config)

        func = "test"
        task = "task 1"
        variant = "variant 1"
        dependencies = ["dep task 1", "dep task 2"]
        task_list.add_task(task, [under_test._cmd_by_name(func)], depends_on=dependencies)
        task_list.add_to_variant(variant)

        cfg_dict = config.to_map()

        cmd_dict = cfg_dict["tasks"][0]
        self.assertEqual(task, cmd_dict["name"])
        self.assertEqual(func, cmd_dict["commands"][0]["func"])
        for dep in dependencies:
            self.assertIn(dep, {d["name"] for d in cmd_dict["depends_on"]})

        task_dict = cfg_dict["buildvariants"][0]["tasks"][0]
        self.assertEqual(task, task_dict["name"])

    def test_adding_multiple_tasks(self):
        config = Configuration()
        task_list = under_test.TaskList(config)

        func = "test"
        variant = "variant 1"
        tasks = ["task 1", "task 2"]
        for task in tasks:
            task_list.add_task(task, [under_test._cmd_by_name(func)])

        task_list.add_to_variant(variant)

        cfg_dict = config.to_map()

        self.assertEqual(len(tasks), len(cfg_dict["tasks"]))
        self.assertEqual(len(tasks), len(cfg_dict["buildvariants"][0]["tasks"]))

    def test_using_display_task(self):
        config = Configuration()
        task_list = under_test.TaskList(config)

        func = "test"
        variant = "variant 1"
        tasks = ["task 1", "task 2"]
        for task in tasks:
            task_list.add_task(task, [under_test._cmd_by_name(func)])

        display_task = "display_task"
        task_list.add_to_variant(variant, display_task)

        cfg_dict = config.to_map()

        self.assertEqual(len(tasks), len(cfg_dict["tasks"]))
        variant_dict = cfg_dict["buildvariants"][0]
        self.assertEqual(len(tasks), len(variant_dict["tasks"]))
        dt_dict = variant_dict["display_tasks"][0]
        self.assertEqual(display_task, dt_dict["name"])
        for task in tasks:
            self.assertIn(task, dt_dict["execution_tasks"])

    def test_using_display_task_with_existing_tasks(self):
        config = Configuration()
        task_list = under_test.TaskList(config)

        func = "test"
        variant = "variant 1"
        tasks = ["task 1", "task 2"]
        for task in tasks:
            task_list.add_task(task, [under_test._cmd_by_name(func)])

        display_task = "display_task"
        existing_tasks = ["other task 1", "other task 2"]
        task_list.add_to_variant(variant, display_task, existing_tasks)

        cfg_dict = config.to_map()

        self.assertEqual(len(tasks), len(cfg_dict["tasks"]))
        variant_dict = cfg_dict["buildvariants"][0]
        self.assertEqual(len(tasks), len(variant_dict["tasks"]))
        dt_dict = variant_dict["display_tasks"][0]
        self.assertEqual(display_task, dt_dict["name"])
        for task in tasks:
            self.assertIn(task, dt_dict["execution_tasks"])
        for task in existing_tasks:
            self.assertIn(task, dt_dict["execution_tasks"])
