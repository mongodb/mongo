"""Unit tests for the evergreen_gen_fuzzer_tests.py script."""

import unittest
import mock

from shrub.v2 import BuildVariant, ShrubProject

from buildscripts import evergreen_gen_fuzzer_tests as under_test

# pylint: disable=missing-docstring,protected-access


class TestCreateFuzzerTask(unittest.TestCase):
    @staticmethod
    def _create_options_mock():
        options = mock.Mock(spec=under_test.ConfigOptions)
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
        options.suite = "test_suite"

        return options

    def test_evg_config_is_created_without_multiversion(self):
        build_variant = BuildVariant("build variant")
        options = self._create_options_mock()

        under_test.create_fuzzer_task(options, build_variant)
        shrub_project = ShrubProject.empty().add_build_variant(build_variant)
        config = shrub_project.as_dict()

        self.assertEqual(options.num_tasks, len(config["tasks"]))

        self.assertEqual("setup jstestfuzz", config["tasks"][0]["commands"][1]["func"])

        command1 = config["tasks"][0]["commands"][2]
        self.assertIn(str(options.num_files), command1["vars"]["jstestfuzz_vars"])
        self.assertIn(options.npm_command, command1["vars"]["npm_command"])
        self.assertEqual("run jstestfuzz", command1["func"])

        buildvariant = config["buildvariants"][0]
        self.assertEqual(options.variant, buildvariant["name"])
        self.assertEqual(options.num_tasks, len(buildvariant["tasks"]))
        self.assertEqual(1, len(buildvariant["display_tasks"][0]["execution_tasks"]))
        self.assertEqual(under_test.GEN_PARENT_TASK, buildvariant["display_tasks"][0]["name"])
        self.assertIn(options.name + "_gen", buildvariant["display_tasks"][0]["execution_tasks"])
        self.assertEqual(options.num_tasks,
                         len(buildvariant["display_tasks"][1]["execution_tasks"]))
        self.assertEqual(options.name, buildvariant["display_tasks"][1]["name"])

    def test_evg_config_is_created_with_multiversion(self):
        build_variant = BuildVariant("build variant")
        options = self._create_options_mock()
        options.use_multiversion = "/data/multiversion"

        under_test.create_fuzzer_task(options, build_variant)
        shrub_project = ShrubProject.empty().add_build_variant(build_variant)
        config = shrub_project.as_dict()

        self.assertEqual("do multiversion setup", config["tasks"][0]["commands"][2]["func"])
        self.assertEqual("/data/multiversion",
                         config["tasks"][0]["commands"][5]["vars"]["task_path_suffix"])

    def test_with_large_distro(self):
        build_variant = BuildVariant("build variant")
        options = self._create_options_mock()
        options.large_distro_name = "large build variant"
        options.use_large_distro = True

        under_test.create_fuzzer_task(options, build_variant)
        shrub_project = ShrubProject.empty().add_build_variant(build_variant)
        config = shrub_project.as_dict()

        for variant in config["buildvariants"]:
            for task in variant["tasks"]:
                self.assertEqual(task["distros"], [options.large_distro_name])
