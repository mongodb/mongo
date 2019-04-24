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


class TestTask(unittest.TestCase):
    """Unit tests for the Task class."""

    def test_from_dict(self):
        task_dict = {
            "name":
                "compile", "depends_on": [],
            "commands": [{"func": "fetch source"}, {"func": "run a task that passes"},
                         {"func": "run a function with an arg", "vars": {"foobar": "TESTING: ONE"}},
                         {"func": "run a function with an arg", "vars": {"foobar": "TESTING: TWO"}}]
        }
        task = _evergreen.Task(task_dict)

        self.assertEqual("compile", task.name)
        self.assertEqual([], task.depends_on)
        self.assertEqual(task_dict, task.raw)

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
        self.assertEqual("core", task.resmoke_suite)

    def test_resmoke_args_gen(self):
        task_dict = {
            "name":
                "jsCore", "commands": [{
                    "func": "generate resmoke tasks",
                    "vars": {"task": "core", "resmoke_args": "--shellWriteMode=commands"}
                }]
        }
        task = _evergreen.Task(task_dict)

        self.assertEqual("--suites=core --shellWriteMode=commands", task.resmoke_args)
        self.assertEqual("core", task.resmoke_suite)

    def test_resmoke_args_gen_with_suite(self):
        task_dict = {
            "name":
                "jsCore", "commands": [{
                    "func": "generate resmoke tasks", "vars": {
                        "task": "jsCore", "suite": "core",
                        "resmoke_args": "--shellWriteMode=commands"
                    }
                }]
        }
        task = _evergreen.Task(task_dict)

        self.assertEqual("--suites=core --shellWriteMode=commands", task.resmoke_args)
        self.assertEqual("core", task.resmoke_suite)


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
        self.assertEqual("core", task.resmoke_suite)


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
