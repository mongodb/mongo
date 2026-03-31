"""The unittest.TestCase for Python unittests."""
import os
import sys

from buildscripts.resmokelib import core
from buildscripts.resmokelib.testing.testcases import interface


class PyTestCase(interface.ProcessTestCase):
    """A python test to execute."""

    REGISTERED_NAME = "py_test"

    def __init__(self, logger, py_filename, **kwargs):
        """Initialize PyTestCase."""
        interface.ProcessTestCase.__init__(self, logger, "PyTest", py_filename, **kwargs)

    def _make_process(self):
        program_options = {}
        # Merge test and fixture environment variables into program_options
        self._merge_environment_variables(program_options)
        return core.programs.generic_program(
            self.logger,
            [sys.executable, "-m", "unittest", self.test_name],
            program_options,
        )
