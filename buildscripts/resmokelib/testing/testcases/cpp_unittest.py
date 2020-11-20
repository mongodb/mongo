"""The unittest.TestCase for C++ unit tests."""

from buildscripts.resmokelib import core
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import interface


class CPPUnitTestCase(interface.ProcessTestCase):
    """A C++ unit test to execute."""

    REGISTERED_NAME = "cpp_unit_test"

    def __init__(self, logger, program_executable, program_options=None):
        """Initialize the CPPUnitTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "C++ unit test", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def _make_process(self):
        self.program_options["job_num"] = self.fixture.job_num
        self.program_options["test_id"] = self._id
        return core.programs.make_process(self.logger, [self.program_executable],
                                          **self.program_options)
