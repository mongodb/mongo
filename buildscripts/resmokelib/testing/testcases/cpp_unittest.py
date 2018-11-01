"""The unittest.TestCase for C++ unit tests."""

from __future__ import absolute_import

from . import interface
from ... import core
from ... import utils


class CPPUnitTestCase(interface.ProcessTestCase):
    """A C++ unit test to execute."""

    REGISTERED_NAME = "cpp_unit_test"

    def __init__(self, logger, program_executable, program_options=None):
        """Initialize the CPPUnitTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "C++ unit test", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def _make_process(self):
        return core.programs.make_process(self.logger, [self.program_executable],
                                          **self.program_options)
