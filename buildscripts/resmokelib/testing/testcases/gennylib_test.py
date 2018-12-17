"""The unittest.TestCase for gennylib integration tests."""

from __future__ import absolute_import

from . import interface
from ... import core
from ... import utils


class GennyLibTestCase(interface.ProcessTestCase):
    """A gennylib integration test to be executed.

    The fixture connection string is passed as an environment variable.
    """

    REGISTERED_NAME = "gennylib_test"

    def __init__(self, logger, test_name, program_executable=None, tags=None):
        """Initialize the GennyLibTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "gennylib test", program_executable)

        self.program_executable = program_executable
        self.program_options = {}
        self.tags = tags

    def configure(self, fixture, *args, **kwargs):
        """Configure the test case."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        env_vars = {"MONGO_CONNECTION_STRING": self.fixture.get_driver_connection_url()}
        process_kwargs = {"env_vars": env_vars}
        self.program_options["process_kwargs"] = process_kwargs

    def _make_process(self):
        return core.programs.generic_program(self.logger, [self.program_executable, self.tags],
                                             **self.program_options)
