"""
Interface for customizing the behavior of a test fixture by executing a
JavaScript file.
"""

from __future__ import absolute_import

import sys

import pymongo
import pymongo.errors

from . import interface
from ..testcases import jstest
from ... import errors
from ...utils import registry


class JsCustomBehavior(interface.CustomBehavior):
    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, hook_logger, fixture, js_filename, description, shell_options=None):
        interface.CustomBehavior.__init__(self, hook_logger, fixture, description)
        self.hook_test_case = self.make_dynamic_test(jstest.JSTestCase,
                                                     js_filename,
                                                     shell_options=shell_options,
                                                     test_kind="Hook")
        self.test_case_is_configured = False

    def before_suite(self, test_report):
        if not self.test_case_is_configured:
            # Configure the test case after the fixture has been set up.
            self.hook_test_case.configure(self.fixture)
            self.test_case_is_configured = True

    def _should_run_after_test_impl(self):
        return True

    def _after_test_impl(self, test, test_report, description):
        self.hook_test_case.run_test()

    def after_test(self, test, test_report):
        if not self._should_run_after_test_impl():
            return

        # Change test_name and description to be more descriptive.
        description = "{0} after running '{1}'".format(self.description, test.short_name())
        test_name = "{}:{}".format(test.short_name(), self.__class__.__name__)
        self.hook_test_case.test_name = test_name

        interface.CustomBehavior.start_dynamic_test(self.hook_test_case, test_report)
        try:
            self._after_test_impl(test, test_report, description)
        except pymongo.errors.OperationFailure as err:
            self.hook_test_case.logger.exception("{0} failed".format(description))
            self.hook_test_case.return_code = 1
            test_report.addFailure(self.hook_test_case, sys.exc_info())
            raise errors.StopExecution(err.args[0])
        except self.hook_test_case.failureException as err:
            self.hook_test_case.logger.error("{0} failed".format(description))
            test_report.addFailure(self.hook_test_case, sys.exc_info())
            raise errors.StopExecution(err.args[0])
        else:
            self.hook_test_case.return_code = 0
            test_report.addSuccess(self.hook_test_case)
        finally:
            test_report.stopTest(self.hook_test_case)
