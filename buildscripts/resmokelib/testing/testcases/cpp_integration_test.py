"""The unittest.TestCase for C++ integration tests."""

from . import interface
from ... import core
from ... import utils


class CPPIntegrationTestCase(interface.ProcessTestCase):
    """A C++ integration test to execute."""

    REGISTERED_NAME = "cpp_integration_test"

    def __init__(self, logger, program_executable, program_options=None):
        """Initialize the CPPIntegrationTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "C++ integration test", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def configure(self, fixture, *args, **kwargs):
        """Configure the test case."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        self.program_options["connectionString"] = self.fixture.get_internal_connection_string()

    def _make_process(self):
        return core.programs.generic_program(self.logger, [self.program_executable],
                                             **self.program_options)
