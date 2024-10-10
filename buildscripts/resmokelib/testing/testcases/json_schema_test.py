"""The unittest.TestCase for JSON Schema tests."""

from typing import Optional

from buildscripts.resmokelib import logging
from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class JSONSchemaTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """A JSON Schema test to execute."""

    REGISTERED_NAME = "json_schema_test"

    def __init__(
        self,
        logger: logging.Logger,
        json_filenames: list[str],
        shell_executable: Optional[str] = None,
        shell_options: Optional[dict] = None,
    ):
        """Initialize the JSONSchemaTestCase with the JSON test file."""

        assert len(json_filenames) == 1

        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self,
            logger,
            "JSON Schema test",
            json_filenames[0],
            test_runner_file="jstests/libs/json_schema_test_runner.js",
            shell_executable=shell_executable,
            shell_options=shell_options,
        )

    @property
    def json_filename(self):
        """Get the JSON filename."""
        return self.test_name

    def _populate_test_data(self, test_data):
        test_data["jsonSchemaTestFile"] = self.json_filename
        test_data["peerPids"] = self.fixture.pids()
