"""The unittest.TestCase for C++ unit tests."""

import os
from typing import Optional

from buildscripts.resmokelib import config, core, logging, utils
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

    def run_test(self):
        """Run the test."""
        try:
            super().run_test()
        except self.failureException:
            if config.UNDO_RECORDER_PATH:
                # Record the list of failed tests so we can upload them to the Evergreen task.
                # Non-recorded tests rely on the core dump content to identify the test binaries.
                with open("failed_recorded_tests.txt", "a") as failure_list:
                    failure_list.write(self.program_executable)
                    failure_list.write("\n")
                self.logger.exception(
                    "*** Failed test run was recorded. ***\n"
                    "For instructions on using the recording instead of core dumps, see\n"
                    "https://wiki.corp.mongodb.com/display/COREENG/Time+Travel+Debugging+in+MongoDB\n"
                    "For questions or bug reports, please reach out in #server-testing"
                )

                # Archive any available recordings if there's any failure. It's possible a problem
                # with the recorder will cause no recordings to be generated.
                self._cull_recordings(os.path.basename(self.program_executable))
            raise

    def _make_process(self):
        return core.programs.make_process(
            self.logger, [self.program_executable], **self.program_options
        )
