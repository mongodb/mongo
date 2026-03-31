"""Test hook for external module."""

from buildscripts.resmokelib.testing.hooks import interface


class TestExternalHook(interface.Hook):
    """A simple test hook for external module testing."""

    REGISTERED_NAME = "TestExternalHook"
    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, description):
        interface.Hook.__init__(self, hook_logger, fixture, description)

    def before_suite(self, test_report):
        """Run before the suite."""
        pass

    def after_suite(self, test_report, teardown_flag=None):
        """Run after the suite."""
        pass

    def before_test(self, test, test_report):
        """Run before each test."""
        pass

    def after_test(self, test, test_report):
        """Run after each test."""
        pass
