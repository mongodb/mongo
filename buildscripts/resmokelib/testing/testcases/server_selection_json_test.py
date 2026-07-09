"""The unittest.TestCase for Server Selection JSON tests."""

import os
import os.path
import shutil
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
        program_executable: Optional[str] = None,
        program_options: Optional[dict] = None,
        **kwargs,
    ):
        """Initialize the TestCase with the executable to run."""
        assert len(json_test_files) == 1
        interface.ProcessTestCase.__init__(
            self, logger, "Server Selection Json Test", json_test_files[0], **kwargs
        )

        if program_executable:
            self.program_executable = program_executable
        else:
            self.program_executable = self._find_executable()
        self.json_test_file = os.path.normpath(json_test_files[0])
        self.program_options = utils.default_if_none(program_options, {}).copy()

        interface.append_process_tracking_options(self.program_options, self._id)

    def _find_executable(self):
        binary_name = "server_selection_json_test"
        if os.name == "nt":
            binary_name += ".exe"

        # When run with an install dir (e.g. `resmoke.py run --installDir=...`),
        # look there first.
        if config.INSTALL_DIR:
            binary = os.path.join(config.INSTALL_DIR, binary_name)
            if os.path.isfile(binary):
                return binary

        # Otherwise fall back to PATH.
        binary = shutil.which(binary_name)
        if binary:
            return binary

        raise errors.StopExecution(f"Failed to locate {binary_name} binary")

    def _make_process(self):
        command_line = [self.program_executable]
        command_line += ["--source-dir", self.TEST_DIR]
        command_line += ["-f", self.json_test_file]
        # Merge test and fixture environment variables into program_options
        program_options = self.program_options.copy()
        self._merge_environment_variables(program_options)
        return core.programs.make_process(self.logger, command_line, **program_options)
