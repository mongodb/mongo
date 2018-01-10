"""
Interface for customizing the behavior of a test fixture.
"""

from __future__ import absolute_import

from ...logging import loggers
from ...utils import registry


_HOOKS = {}


def make_custom_behavior(class_name, *args, **kwargs):
    """
    Factory function for creating CustomBehavior instances.
    """

    if class_name not in _HOOKS:
        raise ValueError("Unknown custom behavior class '%s'" % (class_name))

    return _HOOKS[class_name](*args, **kwargs)


class CustomBehavior(object):
    """
    The common interface all CustomBehaviors will inherit from.
    """

    __metaclass__ = registry.make_registry_metaclass(_HOOKS)

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    @staticmethod
    def start_dynamic_test(hook_test_case, test_report):
        """
        If a CustomBehavior wants to add a test case that will show up
        in the test report, it should use this method to add it to the
        report, since we will need to count it as a dynamic test to get
        the stats in the summary information right.
        """
        test_report.startTest(hook_test_case, dynamic=True)

    def __init__(self, hook_logger, fixture, description):
        """
        Initializes the CustomBehavior with the specified fixture.
        """

        if not isinstance(hook_logger, loggers.HookLogger):
            raise TypeError("logger must be a HookLogger instance")

        self.logger = hook_logger
        self.fixture = fixture
        self.description = description
        self.hook_test_case = None

    def make_dynamic_test(self, test_case_class, *args, **kwargs):
        """
        Returns an instance of 'test_case_class' configured to use the
        appropriate logger.
        """
        return test_case_class(self.logger.test_case_logger, *args, **kwargs)

    def before_suite(self, test_report):
        """
        The test runner calls this exactly once before they start
        running the suite.
        """
        pass

    def after_suite(self, test_report):
        """
        The test runner calls this exactly once after all tests have
        finished executing. Be sure to reset the behavior back to its
        original state so that it can be run again.
        """
        pass

    def before_test(self, test, test_report):
        """
        Each test will call this before it executes.
        """
        pass

    def after_test(self, test, test_report):
        """
        Each test will call this after it executes.
        """
        pass
