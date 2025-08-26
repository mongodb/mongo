"""The unittest.TestCase for Server Discovery and Monitoring JSON tests."""

import os
import os.path
from typing import Optional

from buildscripts.resmokelib import config, core, errors, logging, utils
from buildscripts.resmokelib.testing.testcases import interface


class SDAMJsonTestCase(interface.ProcessTestCase):
    """Server Discovery and Monitoring JSON test case."""

    REGISTERED_NAME = "sdam_json_test"
    TEST_DIR = os.path.normpath("src/mongo/client/sdam/json_tests/sdam_tests")

    def __init__(
        self,
        logger: logging.Logger,
        json_test_files: list[str],
        program_executable: Optional[str] = None,
        program_options: Optional[dict] = None,
    ):
        """Initialize the TestCase with the executable to run."""
        assert len(json_test_files) == 1
        interface.ProcessTestCase.__init__(self, logger, "SDAM Json Test", json_test_files[0])

        if program_executable:
            self.program_executable = program_executable
        else:
            self.program_executable = self._find_executable()
        self.json_test_file = os.path.normpath(json_test_files[0])
        self.program_options = utils.default_if_none(program_options, {}).copy()

        interface.append_process_tracking_options(self.program_options, self._id)


    def _find_executable(self):
        binary = os.path.join(config.INSTALL_DIR, "sdam_json_test")
        if os.name == "nt":
            binary += ".exe"

        if not os.path.isfile(binary):
            raise errors.StopExecution(f"Failed to locate sdam_json_test binary at {binary}")
        return binary

    def _make_process(self):
        command_line = [self.program_executable]
        command_line += ["--source-dir", self.TEST_DIR]
        command_line += ["-f", self.json_test_file]
        return core.programs.make_process(self.logger, command_line, **self.program_options)
