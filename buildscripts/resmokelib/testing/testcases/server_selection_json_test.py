"""The unittest.TestCase for Server Selection JSON tests."""
import os
import os.path
from . import interface
from ... import core
from ... import config
from ... import utils
from ...utils import globstar
from ... import errors


class ServerSelectionJsonTestCase(interface.ProcessTestCase):
    """Server Selection JSON test case."""

    REGISTERED_NAME = "server_selection_json_test"
    EXECUTABLE_BUILD_PATH = "build/**/mongo/client/sdam/server_selection_json_test"
    TEST_DIR = os.path.normpath("src/mongo/client/sdam/json_tests/server_selection_tests")

    def __init__(self, logger, json_test_file, program_options=None):
        """Initialize the TestCase with the executable to run."""
        interface.ProcessTestCase.__init__(self, logger, "Server Selection Json Test",
                                           json_test_file)

        self.program_executable = self._find_executable()
        self.json_test_file = os.path.normpath(json_test_file)
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def _find_executable(self):
        if config.INSTALL_DIR is not None:
            binary = os.path.join(config.INSTALL_DIR, "server_selection_json_test")
            if os.name == "nt":
                binary += ".exe"

            if os.path.isfile(binary):
                return binary

        execs = globstar.glob(self.EXECUTABLE_BUILD_PATH + '.exe')
        if not execs:
            execs = globstar.glob(self.EXECUTABLE_BUILD_PATH)
        if len(execs) != 1:
            raise errors.StopExecution(
                "There must be a single server_selection_json_test binary in {}".format(execs))
        return execs[0]

    def _make_process(self):
        command_line = [self.program_executable]
        command_line += ["--source-dir", self.TEST_DIR]
        command_line += ["-f", self.json_test_file]
        return core.programs.make_process(self.logger, command_line)
