"""The unittest.TestCase to run tests in a try/catch block."""

from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class MagicRestoreTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """A test to execute for running tests in a try/catch block."""

    REGISTERED_NAME = "magic_restore_js_test"

    def __init__(self, logger, js_filenames: list[str], shell_executable=None, shell_options=None):
        """Initialize the MagicRestoreTestCase."""
        assert len(js_filenames) == 1
        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self,
            logger,
            "MagicRestore Test",
            js_filenames[0],
            test_runner_file="src/mongo/db/modules/enterprise/jstests/hot_backups/libs/magic_restore_passthrough_runner.js",
            shell_executable=shell_executable,
            shell_options=shell_options,
        )

    @property
    def js_filename(self):
        """Return the name of the test file."""
        return self.test_name

    def _populate_test_data(self, test_data):
        test_data["jsTestFile"] = self.js_filename
