"""The unittest.TestCase for pretty printer tests."""
import os

from buildscripts.resmokelib import config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import interface


class PrettyPrinterTestCase(interface.ProcessTestCase):
    """A pretty printer test to execute."""

    REGISTERED_NAME = "pretty_printer_test"

    def __init__(self, logger, program_executable, program_options=None):
        """Initialize the PrettyPrinterTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "pretty printer test", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def _make_process(self):
        return core.programs.make_process(self.logger, [self.program_executable],
                                          **self.program_options)
