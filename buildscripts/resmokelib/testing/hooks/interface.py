"""Interface for customizing the behavior of a test fixture."""

from __future__ import absolute_import

import sys

from ..testcases import interface as testcase
from ... import errors
from ...logging import loggers
from ...utils import registry

_HOOKS = {}  # type: ignore


def make_hook(class_name, *args, **kwargs):
    """Provide factory function for creating Hook instances."""

    if class_name not in _HOOKS:
        raise ValueError("Unknown hook class '%s'" % class_name)

    return _HOOKS[class_name](*args, **kwargs)


class Hook(object):
    """Common interface all Hooks will inherit from."""

    __metaclass__ = registry.make_registry_metaclass(_HOOKS)  # type: ignore

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, hook_logger, fixture, description):
        """Initialize the Hook with the specified fixture."""

        if not isinstance(hook_logger, loggers.HookLogger):
            raise TypeError("logger must be a HookLogger instance")

        self.logger = hook_logger
        self.fixture = fixture
        self.description = description

    def before_suite(self, test_report):
        """Test runner calls this exactly once before they start running the suite."""
        pass

    def after_suite(self, test_report):
        """Invoke by test runner calls this exactly once after all tests have finished executing.

        Be sure to reset the behavior back to its original state so that it can be run again.
        """
        pass

    def before_test(self, test, test_report):
        """Each test will call this before it executes."""
        pass

    def after_test(self, test, test_report):
        """Each test will call this after it executes."""
        pass


class DynamicTestCase(testcase.TestCase):  # pylint: disable=abstract-method
    """DynamicTestCase class."""

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, test_name, description, base_test_name, hook):
        """Initialize DynamicTestCase."""
        testcase.TestCase.__init__(self, logger, "Hook", test_name)
        self.description = description
        self._hook = hook
        self._base_test_name = base_test_name

    def run_dynamic_test(self, test_report):
        """Provide helper method to run a dynamic test and update the test report."""
        test_report.startTest(self, dynamic=True)
        try:
            self.run_test()
        except (errors.TestFailure, self.failureException) as err:
            self.return_code = 1
            self.logger.exception("{0} failed".format(self.description))
            test_report.addFailure(self, sys.exc_info())
            raise errors.TestFailure(err.args[0])
        except:
            self.return_code = 2
            test_report.addFailure(self, sys.exc_info())
            raise
        else:
            self.return_code = 0
            test_report.addSuccess(self)
        finally:
            test_report.stopTest(self)

    def as_command(self):
        """Provide base method."""
        return "(dynamic test case)"

    @classmethod
    def create_before_test(cls, logger, base_test, hook, *args, **kwargs):
        """Create a hook dynamic test to be run before an existing test."""
        base_test_name = base_test.short_name()
        test_name = cls._make_test_name(base_test_name, hook)
        description = "{} before running '{}'".format(hook.description, base_test_name)
        return cls(logger, test_name, description, base_test_name, hook, *args, **kwargs)

    @classmethod
    def create_after_test(cls, logger, base_test, hook, *args, **kwargs):
        """Create a hook dynamic test to be run after an existing test."""
        base_test_name = base_test.short_name()
        test_name = cls._make_test_name(base_test_name, hook)
        description = "{} after running '{}'".format(hook.description, base_test_name)
        return cls(logger, test_name, description, base_test_name, hook, *args, **kwargs)

    @staticmethod
    def _make_test_name(base_test_name, hook):
        return "{}:{}".format(base_test_name, hook.__class__.__name__)
