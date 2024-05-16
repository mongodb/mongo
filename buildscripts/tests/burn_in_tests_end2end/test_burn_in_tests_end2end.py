import os
import subprocess
import sys
import unittest

import yaml

import buildscripts.burn_in_tests as under_test


class TestBurnInTestsEnd2End(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(
            [
                sys.executable,
                "buildscripts/burn_in_tests.py",
                "generate-test-membership-map-file-for-ci",
            ]
        )

    @classmethod
    def tearDownClass(cls):
        if os.path.exists(under_test.BURN_IN_TEST_MEMBERSHIP_FILE):
            os.remove(under_test.BURN_IN_TEST_MEMBERSHIP_FILE)

    def test_valid_yaml_output(self):
        process = subprocess.run(
            [
                sys.executable,
                "buildscripts/burn_in_tests.py",
                "run",
                "--yaml",
            ],
            text=True,
            capture_output=True,
        )
        output = process.stdout
        self.assertEqual(0, process.returncode)

        try:
            yaml.safe_load(output)
        except Exception:
            self.fail(msg="burn_in_tests.py does not output valid yaml.")
