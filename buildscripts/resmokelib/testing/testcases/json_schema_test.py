"""
unittest.TestCase for JSON Schema tests.
"""

from __future__ import absolute_import

from . import interface
from ... import config
from ... import core
from ... import utils


class JSONSchemaTestCase(interface.TestCase):
    """
    A JSON Schema test to execute.
    """

    REGISTERED_NAME = "json_schema_test"
    TEST_RUNNER_FILE = "jstests/libs/json_schema_test_runner.js"

    def __init__(self,
                 logger,
                 json_filename,
                 shell_executable=None,
                 shell_options=None):
        """Initializes the JSONSchemaTestCase with the JSON test file."""

        interface.TestCase.__init__(self, logger, "JSON Schema Test", json_filename)

        # Command line options override the YAML configuration.
        self.shell_executable = utils.default_if_none(config.MONGO_EXECUTABLE, shell_executable)

        self.json_filename = json_filename
        self.shell_options = utils.default_if_none(shell_options, {}).copy()

    def configure(self, fixture, *args, **kwargs):
        interface.TestCase.configure(self, fixture, *args, **kwargs)

        global_vars = self.shell_options.get("global_vars", {}).copy()

        test_data = global_vars.get("TestData", {}).copy()
        test_data["jsonSchemaTestFile"] = self.json_filename

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

    def run_test(self):
        try:
            shell = self._make_process()
            self._execute(shell)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running JSON Schema test %s.",
                                  self.basename())
            raise

    def _make_process(self):
        return core.programs.mongo_shell_program(
            self.logger,
            executable=self.shell_executable,
            connection_string=self.fixture.get_driver_connection_url(),
            filename=JSONSchemaTestCase.TEST_RUNNER_FILE,
            **self.shell_options)
