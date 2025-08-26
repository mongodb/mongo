"""The unittest.TestCase for QueryTester self-tests."""

import sys

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import core, logging
from buildscripts.resmokelib.testing.testcases import interface


class QueryTesterSelfTestCase(interface.ProcessTestCase):
    """A QueryTester self-test to execute."""

    REGISTERED_NAME = "query_tester_self_test"

    def __init__(self, logger: logging.Logger, test_filenames: list[str]):
        """Initialize QueryTesterSelfTestCase.

        test_filenames must contain one test_file - a python file that takes one argument: the uri of the mongod.
        To run multiple test files, you would create an instance of QueryTesterSelfTestCase for each one.
        """
        assert len(test_filenames) == 1
        interface.ProcessTestCase.__init__(self, logger, "QueryTesterSelfTest", test_filenames[0])
        self.test_file = test_filenames[0]

    def _make_process(self): 
        program_options = {}
        interface.append_process_tracking_options(program_options, self._id)
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
