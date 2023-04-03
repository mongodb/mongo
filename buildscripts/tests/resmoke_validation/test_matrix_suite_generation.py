import logging
import os
import subprocess
import unittest
from buildscripts.resmokelib import config, suitesconfig
from buildscripts.resmokelib.errors import InvalidMatrixSuiteError
from buildscripts.resmokelib.logging import loggers


class TestSuiteGeneration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        config.CONFIG_DIR = "buildscripts/tests/resmoke_validation"
        cls.matrix_suite_config = suitesconfig.MatrixSuiteConfig()
        loggers.ROOT_EXECUTOR_LOGGER = logging

    def verify_suite_generation(self):
        tested_suite = "test_matrix_suite"

        generated_suite_path = self.matrix_suite_config.get_generated_suite_path(tested_suite)
        if os.path.exists(generated_suite_path):
            os.remove(generated_suite_path)

        with self.assertRaises(
                InvalidMatrixSuiteError, msg=
                f"{tested_suite} suite should have failed because the generated suite does not exist."
        ):
            self.matrix_suite_config.get_config_obj(tested_suite)

        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)

        try:
            suite = self.matrix_suite_config.get_config_obj(tested_suite)
            self.assertIsNotNone(suite, msg=f"{suite} was not found.")
        except Exception as ex:
            self.fail(repr(ex))

    def verify_altered_generated_suite(self):
        tested_suite = "test_matrix_suite"
        generated_suite_path = self.matrix_suite_config.get_generated_suite_path(tested_suite)
        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)
        with open(generated_suite_path, "a") as file:
            file.write("test change")

        with self.assertRaises(
                InvalidMatrixSuiteError, msg=
                f"{tested_suite} suite should have failed because the generated suite was edited."):
            self.matrix_suite_config.get_config_obj(tested_suite)

        # restore original file back
        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)

        try:
            suite = self.matrix_suite_config.get_config_obj(tested_suite)
            self.assertIsNotNone(suite, msg=f"{suite} was not found.")
        except Exception as ex:
            self.fail(repr(ex))

    def run_generated_suite(self):
        tested_suite = "test_matrix_suite"
        generated_suite_path = self.matrix_suite_config.get_generated_suite_path(tested_suite)

        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)

        resmoke_process = subprocess.run(
            ["python3", "buildscripts/resmoke.py", "run", "--suites", generated_suite_path])

        self.assertEqual(0, resmoke_process.returncode,
                         msg="Generated resmoke suite did not execute successfully.")

    def test_everything_sequentially(self):
        self.verify_suite_generation()
        self.verify_altered_generated_suite()
        self.run_generated_suite()
