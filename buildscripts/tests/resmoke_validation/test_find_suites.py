import glob
import subprocess
import unittest

from buildscripts.resmokelib.parser import set_run_options
from buildscripts.resmokelib.suitesconfig import get_suite


class TestFindSuites(unittest.TestCase):
    def test_find_suites(self):
        jstests = glob.glob("jstests/core/testing/*.js")
        resmoke_process = subprocess.run(
            ["python3", "buildscripts/resmoke.py", "find-suites", jstests[0]],
            stdout=subprocess.PIPE,
            text=True,
            check=False,
        )

        self.assertEqual(
            0,
            resmoke_process.returncode,
            # Give a very verbose failure message - this can be read by users well
            # outside of resmoke-areas in case of failures on malformatted yaml configs
            msg=f"find-suites subcommand did not execute successfully:\n\n{resmoke_process.stdout}",
        )

        self.assertTrue(resmoke_process.stdout, msg="find-suites output must not be empty")

        set_run_options()

        # check that find-suites output is a list of suites, one per line.
        for line in resmoke_process.stdout.splitlines():
            suite = get_suite(line)
            self.assertTrue(
                suite,
                msg=f"find-suites output line does not match suite name format: '{line}'",
            )
