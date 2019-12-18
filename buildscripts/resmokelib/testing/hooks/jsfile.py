"""Interface for customizing the behavior of a test fixture by executing a JavaScript file."""

from . import interface
from ..testcases import jstest
from ... import errors
from ...utils import registry


class JSHook(interface.Hook):
    """A hook with a static JavaScript file to execute."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(  # pylint: disable=too-many-arguments
            self, hook_logger, fixture, js_filename, description, shell_options=None):
        """Initialize JSHook."""
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self._js_filename = js_filename
        self._shell_options = shell_options

    def _should_run_after_test(self):  # pylint: disable=no-self-use
        """Provide base callback.

        Callback that can be overrided by subclasses to indicate if the JavaScript file should be
        executed after the current test.
        """
        return True

    def after_test(self, test, test_report):
        """After test execution."""
        if not self._should_run_after_test():
            return

        hook_test_case = DynamicJSTestCase.create_after_test(
            self.logger.test_case_logger, test, self, self._js_filename, self._shell_options)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)


class DataConsistencyHook(JSHook):
    """
    A hook for running a static JavaScript file that checks data consistency of the server.

    If the mongo shell process running the JavaScript file exits with a non-zero return code, then
    an errors.ServerFailure exception is raised to cause resmoke.py's test execution to stop.
    """

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def after_test(self, test, test_report):
        """After test execution."""
        try:
            JSHook.after_test(self, test, test_report)
        except errors.TestFailure as err:
            raise errors.ServerFailure(err.args[0])


class DynamicJSTestCase(interface.DynamicTestCase):
    """A dynamic TestCase that runs a JavaScript file."""

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, test_name, description, base_test_name, hook, js_filename,
            shell_options=None):
        """Initialize DynamicJSTestCase."""
        interface.DynamicTestCase.__init__(self, logger, test_name, description, base_test_name,
                                           hook)
        self._js_test = jstest.JSTestCase(logger, js_filename, shell_options=shell_options)

    def override_logger(self, new_logger):
        """Override logger."""
        interface.DynamicTestCase.override_logger(self, new_logger)
        self._js_test.override_logger(new_logger)

    def reset_logger(self):
        """Reset the logger."""
        interface.DynamicTestCase.reset_logger(self)
        self._js_test.reset_logger()

    def configure(self, fixture, *args, **kwargs):  # pylint: disable=unused-argument
        """Configure the fixture."""
        interface.DynamicTestCase.configure(self, fixture, *args, **kwargs)
        self._js_test.configure(fixture, *args, **kwargs)

    def run_test(self):
        """Execute the test."""
        self._js_test.run_test()
