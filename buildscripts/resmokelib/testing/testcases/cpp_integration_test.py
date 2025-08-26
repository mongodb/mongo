"""The unittest.TestCase for C++ integration tests."""

import copy
from typing import Optional

from buildscripts.resmokelib import core, logging, utils
from buildscripts.resmokelib.testing.testcases import interface


class CPPIntegrationTestCase(interface.ProcessTestCase):
    """A C++ integration test to execute."""

    REGISTERED_NAME = "cpp_integration_test"

    def __init__(
        self,
        logger: logging.Logger,
        program_executables: list[str],
        program_options: Optional[dict] = None,
    ):
        """Initialize the CPPIntegrationTestCase with the executable to run."""

        assert len(program_executables) == 1
        interface.ProcessTestCase.__init__(
            self, logger, "C++ integration test", program_executables[0]
        )

        self.program_executable = program_executables[0]
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def configure(self, fixture, *args, **kwargs):
        """Configure the test case."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        self.program_options["connectionString"] = self.fixture.get_shell_connection_string(
            self.program_options.get("useEgressGRPC")
        )

        process_kwargs = copy.deepcopy(self.program_options.get("process_kwargs", {}))
        interface.append_process_tracking_options(process_kwargs, self._id)
        self.program_options["process_kwargs"] = process_kwargs

    def _make_process(self):
        return core.programs.generic_program(
            self.logger, [self.program_executable], **self.program_options
        )
