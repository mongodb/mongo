import logging
import os
import unittest
from buildscripts.resmokelib import config, suitesconfig
from buildscripts.resmokelib.errors import InvalidMatrixSuiteError
from buildscripts.resmokelib.logging import loggers


class ValidateGeneratedSuites(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        config.CONFIG_DIR = "buildscripts/resmokeconfig"
        cls.matrix_suite_config = suitesconfig.MatrixSuiteConfig()
        loggers.ROOT_EXECUTOR_LOGGER = logging

    def test_generated_suites(self):
        suite_names = self.matrix_suite_config.get_named_suites()

        for suite_name in suite_names:
            try:
                suite = self.matrix_suite_config.get_config_obj(suite_name)
                self.assertIsNotNone(
                    suite, msg=
                    f"{suite_name} was not found. This means either MatrixSuiteConfig.get_named_suites() "
                    + "or MatrixSuiteConfig.get_config_obj() are not working as intended.")
            except Exception as ex:
                self.fail(repr(ex))

    def test_stray_generated_files(self):
        suite_names = set(self.matrix_suite_config.get_named_suites())
        suites_dir = os.path.join(self.matrix_suite_config.get_suites_dir(), "generated_suites")
        generated_files = os.listdir(suites_dir)
        for filename in generated_files:
            (suite_name, ext) = os.path.splitext(filename)
            self.assertEqual(ext, ".yml",
                             msg=f"{filename} has the wrong file extension, expected `.yml`")
            expected_mapping_file = os.path.join(self.matrix_suite_config.get_suites_dir(),
                                                 f"mappings/{suite_name}.yml")
            self.assertIn(
                suite_name, suite_names, msg=
                f"{filename} does not have a correlated mapping file . Make a mapping file or delete it."
                f"You have a generated file {filename} that does not have a corresponding mapping file {expected_mapping_file}. "
                +
                "If you have added a non matrix suite to resmokeconfig/matrix_suites/generated_suites, move it to the resmokeconfig/suites."
                + " If you have removed the mapping file be sure to remove the generated file.")
