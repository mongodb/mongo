"""Test hook for enabling and disabling write conflict failpoints.

The main use case is to ensure that other hooks that will run against the server will not
encounter unexpected failures.
"""

import os.path

from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import jsfile


class EnableSpuriousWriteConflicts(interface.Hook):
    """Toggles write conflicts."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize ToggleWriteConflicts."""
        super().__init__(hook_logger, fixture, "TogglesWTWriteConflictExceptions")
        self._enable_js_filename = os.path.join("jstests", "hooks", "enable_write_conflicts.js")
        self._disable_js_filename = os.path.join("jstests", "hooks", "disable_write_conflicts.js")
        self._shell_options = shell_options

    def before_test(self, test, test_report):
        """Enable WTWriteConflictExceptions."""
        hook_test_case = jsfile.DynamicJSTestCase.create_after_test(
            test.logger, test, self, self._enable_js_filename, self._shell_options)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)

    def after_test(self, test, test_report):
        """Disable WTWriteConflictExceptions."""
        hook_test_case = jsfile.DynamicJSTestCase.create_after_test(
            test.logger, test, self, self._disable_js_filename, self._shell_options)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)
