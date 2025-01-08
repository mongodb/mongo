"""The unittest.TestCase for QueryTester server correctness tests."""

import os

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import core, logging
from buildscripts.resmokelib.testing.testcases import interface


class QueryTesterServerTestCase(interface.ProcessTestCase):
    """A QueryTester server test to execute."""

    REGISTERED_NAME = "query_tester_server_test"

    def __init__(self, logger: logging.Logger, test_dir: list[str]):
        """Initialize QueryTesterServerTestCase.
        test_dir must contain one dir that contains .test files, their corresponding .results and a .coll file
        To run multiple test dirs, you would create an instance of QueryTesterServerTestCase for each one.
        """
        assert len(test_dir) == 1
        interface.ProcessTestCase.__init__(self, logger, "QueryTesterServerTest", test_dir[0])
        self.test_files = [
            os.path.join(test_dir[0], f) for f in os.listdir(test_dir[0]) if f.endswith(".test")
        ]

    def _make_process(self):
        return core.programs.generic_program(
            self.logger,
            [
                _config.DEFAULT_MONGOTEST_EXECUTABLE,
                "--uri",
                self.fixture.get_internal_connection_string(),
                *[cmd for f in self.test_files for cmd in ("-t", f)],
                "--drop",
                "--load",
                "--mode",
                "compare",
                "-v",
                "--diff",
                "plain",
            ],
        )
