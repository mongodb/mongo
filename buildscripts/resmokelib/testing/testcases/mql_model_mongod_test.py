"""The unittest.TestCase for MQL MongoD Model tests."""

import os
import os.path
from typing import Optional

from buildscripts.resmokelib import logging
from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class MqlModelMongodTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """A MQL MongoD Model test to execute."""

    REGISTERED_NAME = "mql_model_mongod_test"

    def __init__(
        self,
        logger: logging.Logger,
        json_filenames: list[str],
        shell_executable: Optional[str] = None,
        shell_options: Optional[dict] = None,
    ):
        """Initialize the MqlModelMongodTestCase with the JSON test file."""

        assert len(json_filenames) == 1
        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self,
            logger,
            "MQL MongoD Model test",
            json_filenames[0],
            test_runner_file="jstests/libs/mql_model_mongod_test_runner.js",
            shell_executable=shell_executable,
            shell_options=shell_options,
        )

    @property
    def json_filename(self):
        """Get the JSON filename."""
        return self.test_name

    def _populate_test_data(self, test_data):
        test_data["peerPids"] = self.fixture.pids()

        test_data["mqlTestFile"] = self.json_filename

        top_level_dirname = os.path.normpath(self.json_filename).split(os.sep)[0]

        # We join() with an empty string to include a trailing slash.
        test_data["mqlRootPath"] = os.path.join(top_level_dirname, "")
