"""
Interface for customizing the behavior of a test fixture by executing a
JavaScript file.
"""

from __future__ import absolute_import


from . import interface
from ..testcases import jstest
from ...utils import registry


class JSHook(interface.Hook):
    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, hook_logger, fixture, js_filename, description, shell_options=None):
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self._js_filename = js_filename
        self._shell_options = shell_options

    def _should_run_after_test(self):  # pylint: disable=no-self-use
        """
        Callback that can be overrided by subclasses to indicate if the JavaScript file should be
         executed after the current test.
        """
        return True

    def after_test(self, test, test_report):
        if not self._should_run_after_test():
            return

        hook_test_case = DynamicJSTestCase.create_after_test(
            self.logger.test_case_logger, test, self, self._js_filename, self._shell_options)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)


class DynamicJSTestCase(interface.DynamicTestCase):
    """A dynamic TestCase that runs a JavaScript file."""
    def __init__(self, logger, test_name, description, base_test_name, hook,
                 js_filename, shell_options=None):
        interface.DynamicTestCase.__init__(self, logger, test_name, description,
                                           base_test_name, hook)
        self._js_test = jstest.JSTestCase(logger, js_filename, shell_options=shell_options)

    def override_logger(self, new_logger):
        interface.DynamicTestCase.override_logger(self, new_logger)
        self._js_test.override_logger(new_logger)

    def reset_logger(self):
        interface.DynamicTestCase.reset_logger(self)
        self._js_test.reset_logger()

    def configure(self, fixture, *args, **kwargs):  # pylint: disable=unused-argument
        interface.DynamicTestCase.configure(self, fixture, *args, **kwargs)
        self._js_test.configure(fixture, *args, **kwargs)

    def run_test(self):
        self._js_test.run_test()
