import glob
import subprocess
import unittest


class TestFindSuites(unittest.TestCase):
    def test_find_suites(self):
        jstests = glob.glob("jstests/core/*.js")
        resmoke_process = subprocess.run(
            ["python3", "buildscripts/resmoke.py", "find-suites", jstests[0]]
        )

        self.assertEqual(
            0,
            resmoke_process.returncode,
            msg="find-suites subcommand did not execute successfully.",
        )
