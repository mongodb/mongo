"""Test hook for verifying multikey catalog consistency across replica set members.

Runs continuously in a background thread while a test is executing. On each iteration it picks an
arbitrary clusterTime, snapshot-reads every member of the replica set (or each shard's replica set
+ the config server in a sharded cluster) at that timestamp, and asserts that the multikey state is
identical across all readable members.

Intended for disaggregated-storage validation. Attached storage clusters are expected to have
timestamp inconsistent multikey state.

Configuration (via the `shell_options` `global_vars` -> `TestData.multikeyHook` keys):
  - max_collections_per_iteration (int | None): cap on collections checked per
    iteration. None means unlimited. Defaults to None.
  - max_wildcard_paths_per_index (int): cap on field paths sampled per wildcard index
    per iteration. Defaults to 10.
"""

import os.path

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.testing.hooks.background_job import (
    _BackgroundJob,
    _ContinuousDynamicJSTestCase,
)


class CheckMultikeyConsistencyInBackground(jsfile.JSHook):
    """A hook for comparing multikey catalog state across replica set members.

    Verifies "timestamp-consistent multikeyness" guarantee by snapshot-reading every member and
    asserting that the multikey state is identical across all readable members.
    """

    IS_BACKGROUND = True

    def __init__(
        self,
        hook_logger,
        fixture,
        shell_options=None,
        max_collections_per_iteration=None,
        max_wildcard_paths_per_index=10,
    ):
        """Initialize CheckMultikeyConsistencyInBackground."""
        description = "Check multikey consistency across replica set members"
        js_filename = os.path.join("jstests", "hooks", "run_check_multikey_consistency.js")

        # Thread the configuration into the JS payload via TestData. Merge with any
        # existing shell_options instead of overwriting.
        merged_shell_options = dict(shell_options or {})
        global_vars = dict(merged_shell_options.get("global_vars", {}))
        test_data = dict(global_vars.get("TestData", {}))
        test_data["multikeyHook"] = {
            "maxCollectionsPerIteration": max_collections_per_iteration,
            "maxWildcardPathsPerIndex": max_wildcard_paths_per_index,
        }
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
        client = self.fixture.mongo_client()
        # The JS payload itself checks `supportsSnapshotReadConcern` per shard, but in
        # the replica set case we can short-circuit here to avoid spinning up the thread
        # at all on unsupported storage engines.
        if not client.is_mongos:
            server_status = client.admin.command("serverStatus")
            if not server_status["storageEngine"].get("supportsSnapshotReadConcern", False):
                self.logger.info(
                    "Not enabling the background check multikey consistency thread because"
                    " '%s' storage engine doesn't support snapshot reads.",
                    server_status["storageEngine"]["name"],
                )
                return

        self._background_job = _BackgroundJob("CheckMultikeyConsistencyInBackground")
        self.logger.info("Starting the background check multikey consistency thread.")
        self._background_job.start()

    def after_suite(self, test_report, teardown_flag=None):
        """Signal the background thread to exit, and wait until it does."""
        if self._background_job is None:
            return
        self.logger.info("Stopping the background check multikey consistency thread.")
        self._background_job.stop()

    def before_test(self, test, test_report):
        """Instruct the background thread to run the check while 'test' is also running."""
        if self._background_job is None:
            return

        hook_test_case = _ContinuousDynamicJSTestCase.create_before_test(
            test.logger, test, self, self._js_filename, self._shell_options
        )
        hook_test_case.configure(self.fixture)

        self.logger.info("Resuming the background check multikey consistency thread.")
        self._background_job.resume(hook_test_case, test_report)

    def after_test(self, test, test_report):
        """Pause the background thread; surface any error as a server failure."""
        if self._background_job is None:
            return

        self.logger.info("Pausing the background check multikey consistency thread.")
        self._background_job.pause()

        if self._background_job.exc_info is not None:
            if isinstance(self._background_job.exc_info[1], errors.TestFailure):
                # The JS payload uses assert(...) on divergence — the shell exits
                # non-zero, which surfaces here as a TestFailure. Promote it to
                # ServerFailure so resmoke stops the suite.
                raise errors.ServerFailure(self._background_job.exc_info[1].args[0])
            else:
                self.logger.error(
                    "Encountered an error inside the background check multikey consistency thread.",
                    exc_info=self._background_job.exc_info,
                )
                raise self._background_job.exc_info[1]
