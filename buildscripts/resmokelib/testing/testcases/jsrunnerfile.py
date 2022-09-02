"""The unittest.TestCase for tests with a static JavaScript runner file."""

from buildscripts.resmokelib import config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import interface
from buildscripts.resmokelib.utils import registry


class JSRunnerFileTestCase(interface.ProcessTestCase):
    """A test case with a static JavaScript runner file to execute."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, test_kind, test_name, test_runner_file, shell_executable=None,
                 shell_options=None):
        """Initialize the JSRunnerFileTestCase with the 'test_name' file."""

        interface.ProcessTestCase.__init__(self, logger, test_kind, test_name)

        # Command line options override the YAML configuration.
        self.shell_executable = utils.default_if_none(config.MONGO_EXECUTABLE, shell_executable)

        self.shell_options = utils.default_if_none(shell_options, {}).copy()
        self.test_runner_file = test_runner_file

    def configure(self, fixture, *args, **kwargs):
        """Configure the js runner."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        global_vars = self.shell_options.get("global_vars", {}).copy()

        test_data = global_vars.get("TestData", {}).copy()
        self._populate_test_data(test_data)

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

    def _populate_test_data(self, test_data):
        """Provide base method.

        This method is intended to be overridden by subclasses in order to define the configuration
        necessary for the static JavaScript runner file.
        """
        pass

    def _make_process(self):
        return core.programs.mongo_shell_program(
            self.logger, executable=self.shell_executable,
            connection_string=self.fixture.get_driver_connection_url(),
            filename=self.test_runner_file, test_filename=self.test_name, **self.shell_options)
