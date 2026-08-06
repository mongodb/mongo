"""Test hook for running background metrics filtering validation.

This hook runs continuously in a background thread while the test is running.
"""

import os.path

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.testing.hooks.background_job import (
    _BackgroundJob,
    _ContinuousDynamicJSTestCase,
)


class MetricsFilteringInBackground(jsfile.JSHook):
    """A hook for running metrics filtering validation while a test is running."""

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, shell_options=None, comment=None):
        """Initialize MetricsFilteringInBackground."""
        description = "Runs metrics filtering validation while a test is running"
        js_filename = os.path.join(
            "src",
            "mongo",
            "db",
            "modules",
            "atlas",
            "jstests",
            "disagg_storage",
            "hooks",
            "observability",
            "run_metrics_filtering_background.js",
        )

        # Thread the configuration into the JS payload via TestData. Merge with any
        # existing shell_options instead of overwriting.
        merged_shell_options = dict(shell_options or {})
        global_vars = dict(merged_shell_options.get("global_vars", {}))
        test_data = dict(global_vars.get("TestData", {}))
        test_data["metricsFilteringComment"] = comment
        global_vars["TestData"] = test_data
        merged_shell_options["global_vars"] = global_vars

        jsfile.JSHook.__init__(
            self,
            hook_logger,
            fixture,
            js_filename,
            description,
            shell_options=merged_shell_options,
        )

        self._background_job = None

    def before_suite(self, test_report):
        """Start the background thread."""
        self._background_job = _BackgroundJob("MetricsFilteringInBackground")
        self.logger.info("Starting the background metrics filtering validation thread.")
        self._background_job.start()

    def after_suite(self, test_report, teardown_flag=None):
        """Signal the background thread to exit, and wait until it does."""
        if self._background_job is None:
            return

        self.logger.info("Stopping the background metrics filtering validation thread.")
        self._background_job.stop()

    def before_test(self, test, test_report):
        """Instruct the background thread to run metrics filtering validation while 'test' is also running."""
        if self._background_job is None:
            return

        hook_test_case = _ContinuousDynamicJSTestCase.create_before_test(
            test.logger, test, self, self._js_filename, self._shell_options
        )
        hook_test_case.configure(self.fixture)

        self.logger.info("Resuming the background metrics filtering validation thread.")
        self._background_job.resume(hook_test_case, test_report)

    def after_test(self, test, test_report):  # noqa: D205,D400
        """Instruct the background thread to stop running metrics filtering validation now that 'test' has
        finished running.
        """
        if self._background_job is None:
            return

        self.logger.info("Pausing the background metrics filtering validation thread.")
        self._background_job.pause()

        if self._background_job.exc_info is not None:
            if isinstance(self._background_job.exc_info[1], errors.TestFailure):
                # If the mongo shell process running the JavaScript file exited with a non-zero
                # return code, then we raise an errors.ServerFailure exception to cause resmoke.py's
                # test execution to stop.
                raise errors.ServerFailure(self._background_job.exc_info[1].args[0])
            else:
                self.logger.error(
                    "Encountered an error inside the background metrics filtering validation thread.",
                    exc_info=self._background_job.exc_info,
                )
                raise self._background_job.exc_info[1]
