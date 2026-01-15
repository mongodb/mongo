"""
Test hook for verifying $queryStats collects expected metrics and can redact query shapes.
"""

import os.path

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import jsfile


class RunQueryStats(jsfile.JSHook):
    """Runs $queryStats after every test, and clears the query stats store before every test."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, allow_feature_not_supported=False):
        """Initialize the RunQueryStats hook.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (replica sets or a sharded cluster).
            allow_feature_not_supported: absorb query stats not enabled errors when calling $queryStats.
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

    def before_test(self, test, test_report):
        """Clear query stats before each test."""
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
