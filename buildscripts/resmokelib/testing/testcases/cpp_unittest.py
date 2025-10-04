"""The unittest.TestCase for C++ unit tests."""

from typing import Optional

from buildscripts.resmokelib import core, logging, utils
from buildscripts.resmokelib.testing.testcases import interface


class CPPUnitTestCase(interface.ProcessTestCase):
    """A C++ unit test to execute."""

    REGISTERED_NAME = "cpp_unit_test"

    def __init__(
        self,
        logger: logging.Logger,
        program_executables: list[str],
        program_options: Optional[dict] = None,
    ):
        """Initialize the CPPUnitTestCase with the executable to run."""

        assert len(program_executables) == 1
        interface.ProcessTestCase.__init__(self, logger, "C++ unit test", program_executables[0])

        self.program_executable = program_executables[0]
        self.program_options = utils.default_if_none(program_options, {}).copy()

        interface.append_process_tracking_options(self.program_options, self._id)

    def _make_process(self):
        return core.programs.make_process(
            self.logger, [self.program_executable], **self.program_options
        )
