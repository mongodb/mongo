"""Unit tests for the evergreen_gen_fuzzer_tests.py script."""

from __future__ import absolute_import

import unittest
import mock

from buildscripts import evergreen_gen_fuzzer_tests as gft

# pylint: disable=missing-docstring,protected-access


class TestGenerateEvgTasks(unittest.TestCase):
    @staticmethod
    def _create_options_mock():
        options = mock.Mock
        options.num_tasks = 15
        options.name = "test_task"
        options.use_multiversion = False
        options.npm_command = "jstestfuzz"
        options.num_files = 314
        options.jstestfuzz_vars = "var 1 var 2"
        options.resmoke_args = "resmoke args"
        options.variant = "build variant"
        options.continue_on_failure = "false"
        options.resmoke_jobs_max = 0
        options.should_shuffle = "false"
        options.timeout_secs = "1800"

        return options

    def test_evg_config_is_created_without_multiversion(self):
        options = self._create_options_mock()

        config = gft._generate_evg_tasks(options).to_map()

        self.assertEqual(options.num_tasks, len(config["tasks"]))

        self.assertEqual("setup jstestfuzz", config["tasks"][0]["commands"][1]["func"])

        command1 = config["tasks"][0]["commands"][2]
        self.assertIn(str(options.num_files), command1["vars"]["jstestfuzz_vars"])
        self.assertIn(options.npm_command, command1["vars"]["npm_command"])
        self.assertEqual("run jstestfuzz", command1["func"])

        buildvariant = config["buildvariants"][0]
        self.assertEqual(options.variant, buildvariant["name"])
        self.assertEqual(options.num_tasks, len(buildvariant["tasks"]))
        self.assertEqual(options.num_tasks + 1,
                         len(buildvariant["display_tasks"][0]["execution_tasks"]))
        self.assertEqual(options.name, buildvariant["display_tasks"][0]["name"])
        self.assertIn(options.name + "_gen", buildvariant["display_tasks"][0]["execution_tasks"])

    def test_evg_config_is_created_with_multiversion(self):
        options = self._create_options_mock()
        options.use_multiversion = "/data/multiversion"

        config = gft._generate_evg_tasks(options).to_map()

        self.assertEqual("do multiversion setup", config["tasks"][0]["commands"][1]["func"])
        self.assertEqual("/data/multiversion",
                         config["tasks"][0]["commands"][4]["vars"]["task_path_suffix"])
