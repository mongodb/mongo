"""Unit tests for the buildscripts.ciconfig.evergreen module."""

from __future__ import absolute_import

import datetime
import os
import unittest

import buildscripts.ciconfig.evergreen as _evergreen

# pylint: disable=missing-docstring,protected-access

TEST_FILE_PATH = os.path.join(os.path.dirname(__file__), "evergreen.yml")


class TestEvergreenProjectConfig(unittest.TestCase):
    """Unit tests for the Evergreen for the EvergreenProjectConfig class."""

    @classmethod
    def setUpClass(cls):
        cls.conf = _evergreen.parse_evergreen_file(TEST_FILE_PATH, evergreen_binary=None)

    def test_invalid_path(self):
        invalid_path = "non_existing_file"
        with self.assertRaises(IOError):
            _evergreen.parse_evergreen_file(invalid_path, evergreen_binary=None)

    def test_list_tasks(self):
        self.assertEqual(6, len(self.conf.tasks))
        self.assertEqual(6, len(self.conf.task_names))
        self.assertIn("compile", self.conf.task_names)
        self.assertIn("passing_test", self.conf.task_names)
        self.assertIn("failing_test", self.conf.task_names)
        self.assertIn("timeout_test", self.conf.task_names)
        self.assertIn("no_lifecycle_task", self.conf.task_names)
        self.assertIn("resmoke_task", self.conf.task_names)

    def test_list_task_groups(self):
        self.assertEqual(1, len(self.conf.task_groups))
        self.assertEqual(1, len(self.conf.task_group_names))
        self.assertIn("tg_1", self.conf.task_group_names)

    def test_list_lifecycle_task_names(self):
        self.assertEqual(5, len(self.conf.lifecycle_task_names))
        self.assertIn("compile", self.conf.task_names)
        self.assertIn("passing_test", self.conf.task_names)
        self.assertIn("failing_test", self.conf.task_names)
        self.assertIn("timeout_test", self.conf.task_names)
        self.assertIn("resmoke_task", self.conf.task_names)

    def test_list_variants(self):
        self.assertEqual(4, len(self.conf.variants))
        self.assertEqual(4, len(self.conf.variant_names))
        self.assertIn("osx-108", self.conf.variant_names)
        self.assertIn("ubuntu", self.conf.variant_names)
        self.assertIn("debian", self.conf.variant_names)
        self.assertIn("amazon", self.conf.variant_names)

    def test_get_variant(self):
        variant = self.conf.get_variant("osx-108")

        self.assertIsNotNone(variant)
        self.assertEqual("osx-108", variant.name)

    def test_list_distro_names(self):
        self.assertEqual(5, len(self.conf.distro_names))
        self.assertIn("localtestdistro", self.conf.distro_names)
        self.assertIn("ubuntu1404-test", self.conf.distro_names)
        self.assertIn("pdp-11", self.conf.distro_names)
        self.assertIn("debian-stretch", self.conf.distro_names)
        self.assertIn("amazon", self.conf.distro_names)


class TestTask(unittest.TestCase):  # pylint: disable=too-many-public-methods
    """Unit tests for the Task class."""

    def test_from_dict(self):
        task_dict = {
            "name": "compile",
            "depends_on": [],
            "commands": [{"func": "fetch source"}, {"func": "run a task that passes"},
                         {"func": "run a function with an arg", "vars": {"foobar": "TESTING: ONE"}},
                         {"func": "run a function with an arg", "vars": {"foobar": "TESTING: TWO"}}]
        }  # yapf: disable
        task = _evergreen.Task(task_dict)

        self.assertEqual("compile", task.name)
        self.assertEqual([], task.depends_on)
        self.assertEqual(task_dict, task.raw)

    def test_resmoke_args(self):
        task_commands = [{
            "func": "run tests",
            "vars": {"resmoke_args": "--suites=core --shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertEqual("--suites=core --shellWriteMode=commands", task.resmoke_args)
        self.assertEqual("core", task.resmoke_suite)

    def test_is_run_tests_task(self):
        task_commands = [{
            "func": "run tests",
            "vars": {"resmoke_args": "--suites=core --shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertTrue(task.is_run_tests_task)
        self.assertFalse(task.is_generate_resmoke_task)

    def test_run_tests_command(self):
        task_commands = [{
            "func": "run tests",
            "vars": {"resmoke_args": "--suites=core --shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertDictEqual(task_commands[0], task.run_tests_command)

    def test_run_tests_multiversion(self):
        multiversion_path = "/data/multiversion"
        task_commands = [{"func": "do multiversion setup"}, {
            "func": "run tests", "vars": {
                "task_path_suffix": multiversion_path,
                "resmoke_args": "--suites=core --shellWriteMode=commands"
            }
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertTrue(task.is_multiversion_task)
        self.assertEqual(task.multiversion_setup_command, {"func": "do multiversion setup"})
        self.assertEqual(multiversion_path, task.multiversion_path)

    def test_run_tests_no_multiversion(self):
        task_commands = [{
            "func": "run tests",
            "vars": {"resmoke_args": "--suites=core --shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertFalse(task.is_multiversion_task)
        self.assertIsNone(task.multiversion_setup_command)
        self.assertIsNone(task.multiversion_path)

    def test_resmoke_args_gen(self):
        task_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks",
            "vars": {"task": task_name, "resmoke_args": "--shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertEqual("--suites=core --shellWriteMode=commands", task.resmoke_args)
        self.assertEqual("core", task.resmoke_suite)

    def test_is_generate_resmoke_task(self):
        task_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks",
            "vars": {"task": task_name, "resmoke_args": "--shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertTrue(task.is_generate_resmoke_task)
        self.assertFalse(task.is_run_tests_task)

    def test_generate_resmoke_tasks_command(self):
        task_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks",
            "vars": {"task": task_name, "resmoke_args": "--shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertDictEqual(task_commands[0], task.generate_resmoke_tasks_command)
        self.assertEqual(task_name, task.get_vars_task_name(task_commands[0]["vars"]))

    def test_resmoke_args_gen_with_suite(self):
        task_name = "jsCore"
        suite_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks", "vars": {
                "task": task_name, "suite": suite_name, "resmoke_args": "--shellWriteMode=commands"
            }
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertEqual("--suites=core --shellWriteMode=commands", task.resmoke_args)
        self.assertEqual("core", task.resmoke_suite)

    def test_tags_with_no_tags(self):
        task_dict = {
            "name": "jsCore",
            "commands": [{
                "func": "run tests",
                "vars": {"resmoke_args": "--suites=core --shellWriteMode=commands"}
            }]
        }  # yapf: disable
        task = _evergreen.Task(task_dict)

        self.assertEqual(0, len(task.tags))

    def test_tags_with_tags(self):
        task_dict = {
            "name": "jsCore",
            "tags": ["tag 0", "tag 1", "tag 2"],
            "commands": [{
                "func": "run tests",
                "vars": {"resmoke_args": "--suites=core --shellWriteMode=commands"}
            }]
        }  # yapf: disable
        task = _evergreen.Task(task_dict)

        tag_set = task.tags
        for tag in task_dict["tags"]:
            self.assertIn(tag, tag_set)
        self.assertEqual(len(task_dict["tags"]), len(tag_set))

    def test_generate_resmoke_tasks_command_with_suite(self):
        task_name = "jsCore"
        suite_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks", "vars": {
                "task": task_name, "suite": suite_name, "resmoke_args": "--shellWriteMode=commands"
            }
        }]
        task_dict = {"name": task_name, "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertDictEqual(task_commands[0], task.generate_resmoke_tasks_command)
        self.assertEqual(task_name, task.get_vars_task_name(task_commands[0]["vars"]))

    def test_gen_resmoke_multiversion(self):
        multiversion_path = "/data/multiversion"
        task_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks", "vars": {
                "task": task_name, "resmoke_args": "--shellWriteMode=commands",
                "use_multiversion": multiversion_path
            }
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertTrue(task.is_multiversion_task)
        self.assertEqual(multiversion_path, task.multiversion_path)

    def test_gen_resmoke_no_multiversion(self):
        task_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks",
            "vars": {"task": task_name, "resmoke_args": "--shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertFalse(task.is_multiversion_task)
        self.assertIsNone(task.multiversion_path)

    def test_get_vars_suite_name_run_tests(self):
        suite_name = "core"
        task_commands = [{
            "func": "run tests",
            "vars": {"resmoke_args": "--suites={} --shellWriteMode=commands".format(suite_name)}
        }]

        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)

        with self.assertRaises(ValueError):
            task.get_vars_suite_name(task_commands[0]["vars"])

    def test_get_vars_suite_name_generate_resmoke_tasks(self):
        task_name = "jsCore"
        suite_name = "core"
        task_commands = [{
            "func": "generate resmoke tasks", "vars": {
                "task": task_name, "suite": suite_name, "resmoke_args": "--shellWriteMode=commands"
            }
        }]
        task_dict = {"name": task_name, "commands": task_commands}
        task = _evergreen.Task(task_dict)

        self.assertEqual(suite_name, task.get_vars_suite_name(task_commands[0]["vars"]))

    def test_get_vars_suite_name_gen_resmoke_no_vars(self):
        task_name = "jsCore"
        task_commands = [{"func": "generate resmoke tasks"}]
        task_dict = {"name": task_name, "commands": task_commands}
        task = _evergreen.Task(task_dict)

        with self.assertRaises(TypeError):
            task.get_vars_suite_name(None)

    def test_get_vars_suite_name_gen_resmoke_notask_nosuite(self):
        task_name = "jsCore"
        task_commands = [{
            "func": "generate resmoke tasks", "vars": {"resmoke_args": "--shellWriteMode=commands"}
        }]
        task_dict = {"name": task_name, "commands": task_commands}
        task = _evergreen.Task(task_dict)

        with self.assertRaises(ValueError):
            task.get_vars_suite_name(task_commands[0]["vars"])

    def test_get_vars_task_name(self):
        task_name = "task_name"
        task_commands = [{
            "func": "generate resmoke tasks",
            "vars": {"task": task_name, "resmoke_args": "--shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)
        self.assertEqual(task_name, task.get_vars_task_name(task_commands[0]["vars"]))

    def test_get_vars_task_name_no_task(self):
        task_commands = [{
            "func": "generate resmoke tasks", "vars": {"resmoke_args": "--shellWriteMode=commands"}
        }]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)
        with self.assertRaises(ValueError):
            task.get_vars_task_name(task_commands[0]["vars"])

    def test_get_vars_task_name_no_vars(self):
        task_commands = [{"func": "generate resmoke tasks"}]
        task_dict = {"name": "jsCore", "commands": task_commands}
        task = _evergreen.Task(task_dict)
        with self.assertRaises(TypeError):
            task.get_vars_task_name(None)


class TestTaskGroup(unittest.TestCase):
    """Unit tests for the TaskGroup class."""

    def test_from_list(self):
        task_group_dict = {
            "name": "my_group", "max_hosts": 3, "tasks": ["task1", "task2"], "setup_task": [],
            "teardown_task": [], "setup_group": [], "teardown_group": [], "timeout": []
        }
        task_group = _evergreen.TaskGroup(task_group_dict)

        self.assertEqual("my_group", task_group.name)
        self.assertEqual(2, len(task_group.tasks))
        self.assertEqual(task_group_dict, task_group.raw)

    def test_resmoke_args(self):
        task_dict = {
            "name":
                "jsCore", "commands": [{
                    "func": "run tests",
                    "vars": {"resmoke_args": "--suites=core --shellWriteMode=commands"}
                }]
        }
        task = _evergreen.Task(task_dict)

        self.assertEqual("--suites=core --shellWriteMode=commands", task.resmoke_args)

    def test_resmoke_suite(self):
        suite = "core"
        task_dict = {
            "name":
                "jsCore", "commands": [{
                    "func": "run tests",
                    "vars": {"resmoke_args": "--suites={} --shellWriteMode=commands".format(suite)}
                }]
        }
        task = _evergreen.Task(task_dict)

        self.assertEqual(suite, task.resmoke_suite)

    def test_resmoke_suite_multiple(self):
        task_dict = {
            "name":
                "jsCore",
            "commands": [{
                "func": "run tests",
                "vars": {"resmoke_args": "--suites=core --suites=core2--shellWriteMode=commands"}
            }]
        }
        task = _evergreen.Task(task_dict)

        with self.assertRaises(RuntimeError):
            self.assertIsNone(task.resmoke_suite)

    def test_resmoke_suite_with_comma(self):
        task_dict = {
            "name":
                "jsCore", "commands": [{
                    "func": "run tests",
                    "vars": {"resmoke_args": "--suites=core,core2--shellWriteMode=commands"}
                }]
        }
        task = _evergreen.Task(task_dict)

        with self.assertRaises(RuntimeError):
            self.assertIsNone(task.resmoke_suite)


class TestVariant(unittest.TestCase):
    """Unit tests for the Variant class."""

    @classmethod
    def setUpClass(cls):
        cls.conf = _evergreen.parse_evergreen_file(TEST_FILE_PATH, evergreen_binary=None)

    def test_from_dict(self):
        task = _evergreen.Task({"name": "compile"})
        tasks_map = {task.name: task}
        task_groups_map = {}
        variant_dict = {
            "name": "ubuntu",
            "display_name": "Ubuntu",
            "run_on": ["ubuntu1404-test"],
            "tasks": [{"name": "compile"}],
        }
        variant = _evergreen.Variant(variant_dict, tasks_map, task_groups_map)

        self.assertEqual("ubuntu", variant.name)
        self.assertEqual("Ubuntu", variant.display_name)
        self.assertEqual(["ubuntu1404-test"], variant.run_on)
        self.assertEqual(1, len(variant.tasks))
        self.assertEqual("compile", variant.tasks[0].name)

    def test_display_name(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        self.assertEqual("Ubuntu", variant_ubuntu.display_name)

        variant_osx = self.conf.get_variant("osx-108")
        self.assertEqual("OSX", variant_osx.display_name)

    def test_batchtime(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        batchtime = datetime.timedelta(minutes=1440)
        self.assertEqual(batchtime, variant_ubuntu.batchtime)

        variant_osx = self.conf.get_variant("osx-108")
        self.assertIsNone(variant_osx.batchtime)

    def test_modules(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        self.assertEqual(["render-module"], variant_ubuntu.modules)

        variant_osx = self.conf.get_variant("osx-108")
        self.assertEqual([], variant_osx.modules)

    def test_run_on(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        self.assertEqual(["ubuntu1404-test"], variant_ubuntu.run_on)

        variant_osx = self.conf.get_variant("osx-108")
        self.assertEqual(["localtestdistro"], variant_osx.run_on)

    def test_distro_names(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        self.assertEqual(set(["ubuntu1404-test", "pdp-11"]), variant_ubuntu.distro_names)

        variant_osx = self.conf.get_variant("osx-108")
        self.assertEqual(set(["localtestdistro"]), variant_osx.distro_names)

    def test_test_flags(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        self.assertEqual("--param=value --ubuntu", variant_ubuntu.test_flags)

        variant_osx = self.conf.get_variant("osx-108")
        self.assertIsNone(variant_osx.test_flags)

    def test_num_jobs_available(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        self.assertIsNone(variant_ubuntu.num_jobs_available)

        variant_osx = self.conf.get_variant("osx-108")
        self.assertEqual(549, variant_osx.num_jobs_available)

    def test_variant_tasks(self):
        variant_ubuntu = self.conf.get_variant("ubuntu")
        self.assertEqual(5, len(variant_ubuntu.tasks))
        for task_name in [
                "compile", "passing_test", "failing_test", "timeout_test", "resmoke_task"
        ]:
            task = variant_ubuntu.get_task(task_name)
            self.assertIsNotNone(task)
            self.assertEqual(variant_ubuntu, task.variant)
            self.assertIn(task_name, variant_ubuntu.task_names)

        # Check combined_resmoke_args when test_flags is set on the variant.
        resmoke_task = variant_ubuntu.get_task("resmoke_task")
        self.assertEqual("--suites=somesuite --storageEngine=mmapv1 --param=value --ubuntu",
                         resmoke_task.combined_resmoke_args)

        # Check combined_resmoke_args when the task doesn't have resmoke_args.
        passing_task = variant_ubuntu.get_task("passing_test")
        self.assertIsNone(passing_task.combined_resmoke_args)

        # Check combined_resmoke_args when test_flags is not set on the variant.
        variant_debian = self.conf.get_variant("debian")
        resmoke_task = variant_debian.get_task("resmoke_task")
        self.assertEqual("--suites=somesuite --storageEngine=mmapv1",
                         resmoke_task.combined_resmoke_args)

        # Check for tasks included in task_groups
        variant_amazon = self.conf.get_variant("amazon")
        self.assertEqual(3, len(variant_amazon.tasks))
        self.assertIn("compile", variant_amazon.task_names)


class TestResmokeArgs(unittest.TestCase):
    """Unit tests for the Variant class."""

    def test_get_arg(self):
        suite_name = "suite1"
        resmoke_args = "--suites={} test1.js".format(suite_name)
        self.assertEqual(suite_name, _evergreen.ResmokeArgs.get_arg(resmoke_args, "suites"))

    def test_get_arg_with_space(self):
        suite_name = "suite1"
        resmoke_args = "--suites {} test1.js".format(suite_name)
        self.assertEqual(suite_name, _evergreen.ResmokeArgs.get_arg(resmoke_args, "suites"))

    def test_get_arg_missing(self):
        suite_name = "suite1"
        resmoke_args = "--otherArg={} test1.js".format(suite_name)
        self.assertIsNone(_evergreen.ResmokeArgs.get_arg(resmoke_args, "suites"))

    def test_get_arg_multiple(self):
        resmoke_args = "--myarg=val1 --myarg=val2 test1.js"
        with self.assertRaises(RuntimeError):
            _evergreen.ResmokeArgs.get_arg(resmoke_args, "myarg")

    def test_get_arg_with_comma(self):
        arg_vals = "val1,val2,val3"
        resmoke_args = "--myarg={} test1.js".format(arg_vals)
        self.assertEqual(arg_vals, _evergreen.ResmokeArgs.get_arg(resmoke_args, "myarg"))

    def test_set_arg(self):
        suite_name = "new_suite"
        resmoke_args = "--suites=old_suite test1.js"
        new_resmoke_args = "--suites={} test1.js".format(suite_name)
        self.assertEqual(new_resmoke_args,
                         _evergreen.ResmokeArgs.get_updated_arg(resmoke_args, "suites", suite_name))

    def test_set_arg_with_space(self):
        suite_name = "new_suite"
        resmoke_args = "--suites old_suite test1.js"
        new_resmoke_args = "--suites={} test1.js".format(suite_name)
        self.assertEqual(new_resmoke_args,
                         _evergreen.ResmokeArgs.get_updated_arg(resmoke_args, "suites", suite_name))

    def test_set_arg_same_suite(self):
        suite_name = "new_suite"
        resmoke_args = "--suites=new_suite test1.js"
        new_resmoke_args = "--suites={} test1.js".format(suite_name)
        self.assertEqual(new_resmoke_args,
                         _evergreen.ResmokeArgs.get_updated_arg(resmoke_args, "suites", suite_name))

    def test_set_arg_no_suite(self):
        suite_name = "new_suite"
        resmoke_args = "--other_arg=val1 test1.js"
        new_resmoke_args = "{} --suites={}".format(resmoke_args, suite_name)
        self.assertEqual(new_resmoke_args,
                         _evergreen.ResmokeArgs.get_updated_arg(resmoke_args, "suites", suite_name))

    def test_set_arg_multiple(self):
        new_val = "myval"
        resmoke_args = "--myarg=val1 --myarg=val2 test1.js"
        with self.assertRaises(RuntimeError):
            _evergreen.ResmokeArgs.get_updated_arg(resmoke_args, "myarg", new_val)
