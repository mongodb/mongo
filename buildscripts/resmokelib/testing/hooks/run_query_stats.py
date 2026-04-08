"""
Test hook for verifying $queryStats collects expected metrics and can redact query shapes.
"""

import os.path

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.utils import jscomment


class RunQueryStats(jsfile.JSHook):
    """Runs $queryStats after every test, and clears the query stats store before every test."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, allow_feature_not_supported=False, skip_tags=None):
        """Initialize the RunQueryStats hook.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (replica sets or a sharded cluster).
            allow_feature_not_supported: absorb query stats not enabled errors when calling $queryStats.
            skip_tags: list of test tags for which this hook should not run. The test still
                executes, but RunQueryStats is skipped for it.
        """
        description = "Read query stats data on all nodes after each test."
        js_filename = os.path.join("jstests", "hooks", "query_integration", "run_query_stats.js")

        # TODO SERVER-116389 remove allow_feature_not_supported.
        shell_options = {
            "eval": f"""
                TestData.allowFeatureNotSupported = {str(allow_feature_not_supported).lower()};
            """
        }

        super().__init__(
            hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
        self.allow_feature_not_supported = allow_feature_not_supported
        self._skip_tags = set(skip_tags) if skip_tags else set()

    def _should_skip_test(self, test):
        """Return True if the test has any of the skip_tags."""
        if not self._skip_tags:
            return False
        test_tags = set(jscomment.get_tags(test.test_name))
        matched = self._skip_tags & test_tags
        if matched:
            self.logger.info(
                "Skipping RunQueryStats for %s due to tags: %s", test.test_name, matched
            )
            return True
        return False

    def before_test(self, test, test_report):
        """Clear query stats before each test."""
        if self._should_skip_test(test):
            return
        js_filename = os.path.join("jstests", "hooks", "query_integration", "run_query_stats.js")
        shell_options = {
            "eval": f"""
                TestData.allowFeatureNotSupported = {str(self.allow_feature_not_supported).lower()};
                TestData.queryStatsOperation = "clear";
            """
        }

        hook_test_case = jsfile.DynamicJSTestCase.create_before_test(
            test.logger, test, self, js_filename, shell_options
        )
        hook_test_case.configure(self.fixture)

        try:
            hook_test_case.run_dynamic_test(test_report)
        except errors.TestFailure as err:
            # Convert test failures to warnings for query stats operations.
            if self.allow_feature_not_supported:
                self.logger.warning(
                    f"Failed to clear query stats (may not be enabled): {err.args[0]}"
                )
            else:
                raise errors.ServerFailure(err.args[0])

    def after_test(self, test, test_report):
        """Verify $queryStats after each test."""
        if self._should_skip_test(test):
            return
        js_filename = os.path.join("jstests", "hooks", "query_integration", "run_query_stats.js")
        shell_options = {
            "eval": f"""
                TestData.allowFeatureNotSupported = {str(self.allow_feature_not_supported).lower()};
                TestData.queryStatsOperation = "verify";
            """
        }

        hook_test_case = jsfile.DynamicJSTestCase.create_after_test(
            test.logger, test, self, js_filename, shell_options
        )
        hook_test_case.configure(self.fixture)

        try:
            hook_test_case.run_dynamic_test(test_report)
        except errors.TestFailure as err:
            # Convert test failures to warnings for query stats operations.
            if self.allow_feature_not_supported:
                self.logger.warning(
                    f"Failed to verify query stats (may not be enabled): {err.args[0]}"
                )
            else:
                raise errors.ServerFailure(err.args[0])
