"""Test hook for verifying data consistency across a replica set using the dbCheck command.

This hook runs continuously in a background thread while the test is running.
"""

import os.path

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.testing.hooks.background_job import (
    _BackgroundJob,
    _ContinuousDynamicJSTestCase,
)


class RunDBCheckInBackground(jsfile.JSHook):
    """A hook for running dbCheck on a replica set while a test is running."""

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize RunDBCheckInBackground."""
        description = "Runs dbCheck on a replica set while a test is running"
        js_filename = os.path.join("jstests", "hooks", "run_dbcheck_background.js")
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )

        self._background_job = None

    def before_suite(self, test_report):
        """Start the background thread."""
        self._background_job = _BackgroundJob("RunDBCheckInBackground")
        self.logger.info("Starting the background check repl dbCheck thread.")
        self._background_job.start()

    def after_suite(self, test_report, teardown_flag=None):
        """Signal the background thread to exit, and wait until it does."""
        if self._background_job is None:
            return

        self.logger.info("Stopping the background check repl dbCheck thread.")
        self._background_job.stop()

    def before_test(self, test, test_report):
        """Instruct the background thread to run the dbCheck check while 'test' is also running."""
        if self._background_job is None:
            return

        hook_test_case = _ContinuousDynamicJSTestCase.create_before_test(
            test.logger, test, self, self._js_filename, self._shell_options
        )
        hook_test_case.configure(self.fixture)

        self.logger.info("Resuming the background check repl dbCheck thread.")
        self._background_job.resume(hook_test_case, test_report)

    def after_test(self, test, test_report):  # noqa: D205,D400
        """Instruct the background thread to stop running the dbCheck check now that 'test' has
        finished running.
        """
        if self._background_job is None:
            return

        self.logger.info("Pausing the background check repl dbCheck thread.")
        self._background_job.pause()

        if self._background_job.exc_info is not None:
            if isinstance(self._background_job.exc_info[1], errors.TestFailure):
                # If the mongo shell process running the JavaScript file exited with a non-zero
                # return code, then we raise an errors.ServerFailure exception to cause resmoke.py's
                # test execution to stop.
                raise errors.ServerFailure(self._background_job.exc_info[1].args[0])
            else:
                self.logger.error(
                    "Encountered an error inside the background check repl dbCheck thread.",
                    exc_info=self._background_job.exc_info,
                )
                raise self._background_job.exc_info[1]
