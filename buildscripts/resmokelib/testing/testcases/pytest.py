"""The unittest.TestCase for Python unittests."""

import sys

from buildscripts.resmokelib import core, logging
from buildscripts.resmokelib.testing.testcases import interface


class PyTestCase(interface.ProcessTestCase):
    """A python test to execute."""

    REGISTERED_NAME = "py_test"

    def __init__(self, logger: logging.Logger, py_filenames: list[str], **kwargs):
        """Initialize PyTestCase."""
        assert len(py_filenames) == 1
        interface.ProcessTestCase.__init__(self, logger, "PyTest", py_filenames[0], **kwargs)

    def _make_process(self):
        program_options = {}
        interface.append_process_tracking_options(program_options, self._id)
        # Merge test and fixture environment variables into program_options
        self._merge_environment_variables(program_options)
        return core.programs.generic_program(
            self.logger, [sys.executable, "-m", "unittest", self.test_name], program_options
        )
