"""The unittest.TestCase for JSON Schema tests."""

from buildscripts.resmokelib import config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class JSONSchemaTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """A JSON Schema test to execute."""

    REGISTERED_NAME = "json_schema_test"

    def __init__(self, logger, json_filename, shell_executable=None, shell_options=None):
        """Initialize the JSONSchemaTestCase with the JSON test file."""

        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self, logger, "JSON Schema test", json_filename,
            test_runner_file="jstests/libs/json_schema_test_runner.js",
            shell_executable=shell_executable, shell_options=shell_options)

    @property
    def json_filename(self):
        """Get the JSON filename."""
        return self.test_name

    def _populate_test_data(self, test_data):
        test_data["jsonSchemaTestFile"] = self.json_filename
        test_data["peerPids"] = self.fixture.pids()
