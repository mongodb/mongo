"""The unittest.TestCase for C++ integration tests."""

import copy

from buildscripts.resmokelib import core, utils
from buildscripts.resmokelib.testing.testcases import interface


class CPPIntegrationTestCase(interface.ProcessTestCase):
    """A C++ integration test to execute."""

    REGISTERED_NAME = "cpp_integration_test"

    def __init__(self, logger, program_executable, program_options=None, **kwargs):
        """Initialize the CPPIntegrationTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(
            self, logger, "C++ integration test", program_executable, **kwargs
        )

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def configure(self, fixture, *args, **kwargs):
        """Configure the test case."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        self.program_options["connectionString"] = (
            self.fixture.get_internal_connection_string()
        )

        process_kwargs = copy.deepcopy(self.program_options.get("process_kwargs", {}))
        # Merge test and fixture environment variables into process_kwargs
        self._merge_environment_variables(process_kwargs)
        self.program_options["process_kwargs"] = process_kwargs

    def _make_process(self):
        return core.programs.generic_program(
            self.logger, [self.program_executable], **self.program_options
        )
