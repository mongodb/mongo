"""Test hook to check the sharding metadata consistency of a sharded cluster."""

import os.path

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.testing.hooks.background_job import _BackgroundJob, _ContinuousDynamicJSTestCase


class CheckMetadataConsistencyInBackground(jsfile.DataConsistencyHook):
    """Check the metadata consistency of a sharded cluster."""

    IS_BACKGROUND = True
    # TODO SERVER-74741: Re-enable metedata consistency check once the command is lock free
    SKIP_TESTS = ['jstests/core/txns/many_txns.js']

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckMetadataConsistencyInBackground."""

        if not isinstance(fixture, shardedcluster.ShardedClusterFixture):
            raise ValueError(f"'fixture' must be an instance of ShardedClusterFixture, but got " \
                             f"{fixture.__class__.__name__}")

        description = "Perform consistency checks between the config database and metadata " \
                      "stored/cached in the shards"
        js_filename = os.path.join("jstests", "hooks", "run_check_metadata_consistency.js")
        super().__init__(hook_logger, fixture, js_filename, description,
                         shell_options=shell_options)

        self._background_job = None

    def before_suite(self, test_report):
        """Start the background thread."""

        self._background_job = _BackgroundJob("CheckMetadataConsistencyInBackground")
        self.logger.info("Starting background metadata consistency checker thread")
        self._background_job.start()

    def after_suite(self, test_report, teardown_flag=None):
        """Signal background metadata consistency checker thread to exit, and wait until it does."""

        if self._background_job is None:
            return

        self.logger.info("Stopping background metadata consistency checker thread")
        self._background_job.stop()

    def before_test(self, test, test_report):  # noqa: D205,D400
        """Instruct background metadata consistency checker thread to run while 'test' is also
        running.
        """

        if self._background_job is None:
            return

        hook_test_case = _ContinuousDynamicJSTestCase.create_before_test(
            test.logger, test, self, self._js_filename, self._shell_options)
        hook_test_case.configure(self.fixture)

        if test.test_name in self.SKIP_TESTS:
            self.logger.info("Metadata consistency check explicitely disabled for {test.test_name}")
            return

        self.logger.info("Resuming background metadata consistency checker thread")
        self._background_job.resume(hook_test_case, test_report)

    def after_test(self, test, test_report):  # noqa: D205,D400
        """Instruct background metadata consistency checker thread to stop running now that 'test'
        has finished running.
        """

        if self._background_job is None:
            return

        if test.test_name in self.SKIP_TESTS:
            return

        self.logger.info("Pausing background metadata consistency checker thread")
        self._background_job.pause()

        if self._background_job.exc_info is not None:
            if isinstance(self._background_job.exc_info[1], errors.TestFailure):
                # If the mongo shell process running the JavaScript file exited with a non-zero
                # return code, then we raise an errors.ServerFailure exception to cause resmoke.py's
                # test execution to stop.
                raise errors.ServerFailure(self._background_job.exc_info[1].args[0])
            else:
                self.logger.error(
                    "Encountered an error inside background metadata consistency checker thread",
                    exc_info=self._background_job.exc_info)
                raise self._background_job.exc_info[1]
