"""The unittest.TestCase for Server Selection JSON tests."""

import os
import os.path
from typing import Optional

from buildscripts.resmokelib import config, core, errors, logging, utils
from buildscripts.resmokelib.testing.testcases import interface


class ServerSelectionJsonTestCase(interface.ProcessTestCase):
    """Server Selection JSON test case."""

    REGISTERED_NAME = "server_selection_json_test"
    TEST_DIR = os.path.normpath("src/mongo/client/sdam/json_tests/server_selection_tests")

    def __init__(
        self,
        logger: logging.Logger,
        json_test_files: list[str],
        program_options: Optional[dict] = None,
    ):
        """Initialize the TestCase with the executable to run."""
        assert len(json_test_files) == 1
        interface.ProcessTestCase.__init__(
            self, logger, "Server Selection Json Test", json_test_files[0]
        )

        self.program_executable = self._find_executable()
        self.json_test_file = os.path.normpath(json_test_files[0])
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def _find_executable(self):
        binary = os.path.join(config.INSTALL_DIR, "server_selection_json_test")
        if os.name == "nt":
            binary += ".exe"

        if not os.path.isfile(binary):
            raise errors.StopExecution(
                f"Failed to locate server_selection_json_test binary at {binary}"
            )
        return binary

    def _make_process(self):
        command_line = [self.program_executable]
        command_line += ["--source-dir", self.TEST_DIR]
        command_line += ["-f", self.json_test_file]
        return core.programs.make_process(self.logger, command_line, **self.program_options)
