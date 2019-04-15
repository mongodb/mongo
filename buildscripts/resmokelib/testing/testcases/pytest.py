"""The unittest.TestCase for Python unittests."""
import os
import sys

from buildscripts.resmokelib import core
from buildscripts.resmokelib.testing.testcases import interface


class PyTestCase(interface.ProcessTestCase):
    """A python test to execute."""

    REGISTERED_NAME = "py_test"

    def __init__(self, logger, py_filename):
        """Initialize PyTestCase."""
        interface.ProcessTestCase.__init__(self, logger, "PyTest", py_filename)

    def _make_process(self):
        return core.programs.generic_program(
            self.logger, [sys.executable, "-m", "unittest", self.test_module_name])

    @property
    def test_module_name(self):
        """Get the dotted module name from the path."""
        return os.path.splitext(self.test_name)[0].replace(os.sep, ".")
