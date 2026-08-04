"""The unittest.TestCase for QueryTester self-tests."""

import os
import sys

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import core, logging
from buildscripts.resmokelib.testing.testcases import interface


class QueryTesterSelfTestCase(interface.ProcessTestCase):
    """A QueryTester self-test to execute."""

    REGISTERED_NAME = "query_tester_self_test"

    def __init__(self, logger: logging.Logger, test_filenames: list[str], **kwargs):
        """Initialize QueryTesterSelfTestCase.

        test_filenames must contain one test_file - a python file that takes one argument: the uri of the mongod.
        To run multiple test files, you would create an instance of QueryTesterSelfTestCase for each one.
        """
        assert len(test_filenames) == 1
        interface.ProcessTestCase.__init__(
            self, logger, "QueryTesterSelfTest", test_filenames[0], **kwargs
        )
        self.test_file = test_filenames[0]

    def _make_process(self):
        # The self-tests import their shared helpers as `testlib.test_utils`, relying on the
        # directory containing the test file being on sys.path. Python normally prepends it
        # automatically, but under Bazel rules_python sets PYTHONSAFEPATH=1, which disables that
        # and is inherited by this subprocess. Put the directory on PYTHONPATH explicitly so the
        # import works regardless of how resmoke was launched.
        test_dir = os.path.dirname(os.path.abspath(self.test_file))
        pythonpath = os.pathsep.join(
            path for path in (test_dir, os.environ.get("PYTHONPATH")) if path
        )
        program_options = {"env_vars": {"PYTHONPATH": pythonpath}}
        interface.append_process_tracking_options(program_options, self._id)
        # Merge test and fixture environment variables into program_options
        self._merge_environment_variables(program_options)
        return core.programs.generic_program(
            self.logger,
            [
                sys.executable,
                self.test_file,
                "-u",
                self.fixture.get_internal_connection_string(),
                "-b",
                _config.DEFAULT_MONGOTEST_EXECUTABLE,
            ],
            program_options,
        )
