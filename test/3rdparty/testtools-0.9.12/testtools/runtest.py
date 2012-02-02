# Copyright (c) 2009-2010 testtools developers. See LICENSE for details.

"""Individual test case execution."""

__all__ = [
    'MultipleExceptions',
    'RunTest',
    ]

import sys

from testtools.testresult import ExtendedToOriginalDecorator


class MultipleExceptions(Exception):
    """Represents many exceptions raised from some operation.

    :ivar args: The sys.exc_info() tuples for each exception.
    """


class RunTest(object):
    """An object to run a test.

    RunTest objects are used to implement the internal logic involved in
    running a test. TestCase.__init__ stores _RunTest as the class of RunTest
    to execute.  Passing the runTest= parameter to TestCase.__init__ allows a
    different RunTest class to be used to execute the test.

    Subclassing or replacing RunTest can be useful to add functionality to the
    way that tests are run in a given project.

    :ivar case: The test case that is to be run.
    :ivar result: The result object a case is reporting to.
    :ivar handlers: A list of (ExceptionClass, handler_function) for
        exceptions that should be caught if raised from the user
        code. Exceptions that are caught are checked against this list in
        first to last order.  There is a catch-all of 'Exception' at the end
        of the list, so to add a new exception to the list, insert it at the
        front (which ensures that it will be checked before any existing base
        classes in the list. If you add multiple exceptions some of which are
        subclasses of each other, add the most specific exceptions last (so
        they come before their parent classes in the list).
    :ivar exception_caught: An object returned when _run_user catches an
        exception.
    :ivar _exceptions: A list of caught exceptions, used to do the single
        reporting of error/failure/skip etc.
    """

    def __init__(self, case, handlers=None):
        """Create a RunTest to run a case.

        :param case: A testtools.TestCase test case object.
        :param handlers: Exception handlers for this RunTest. These are stored
            in self.handlers and can be modified later if needed.
        """
        self.case = case
        self.handlers = handlers or []
        self.exception_caught = object()
        self._exceptions = []

    def run(self, result=None):
        """Run self.case reporting activity to result.

        :param result: Optional testtools.TestResult to report activity to.
        :return: The result object the test was run against.
        """
        if result is None:
            actual_result = self.case.defaultTestResult()
            actual_result.startTestRun()
        else:
            actual_result = result
        try:
            return self._run_one(actual_result)
        finally:
            if result is None:
                actual_result.stopTestRun()

    def _run_one(self, result):
        """Run one test reporting to result.

        :param result: A testtools.TestResult to report activity to.
            This result object is decorated with an ExtendedToOriginalDecorator
            to ensure that the latest TestResult API can be used with
            confidence by client code.
        :return: The result object the test was run against.
        """
        return self._run_prepared_result(ExtendedToOriginalDecorator(result))

    def _run_prepared_result(self, result):
        """Run one test reporting to result.

        :param result: A testtools.TestResult to report activity to.
        :return: The result object the test was run against.
        """
        result.startTest(self.case)
        self.result = result
        try:
            self._exceptions = []
            self._run_core()
            if self._exceptions:
                # One or more caught exceptions, now trigger the test's
                # reporting method for just one.
                e = self._exceptions.pop()
                for exc_class, handler in self.handlers:
                    if isinstance(e, exc_class):
                        handler(self.case, self.result, e)
                        break
        finally:
            result.stopTest(self.case)
        return result

    def _run_core(self):
        """Run the user supplied test code."""
        if self.exception_caught == self._run_user(self.case._run_setup,
            self.result):
            # Don't run the test method if we failed getting here.
            self._run_cleanups(self.result)
            return
        # Run everything from here on in. If any of the methods raise an
        # exception we'll have failed.
        failed = False
        try:
            if self.exception_caught == self._run_user(
                self.case._run_test_method, self.result):
                failed = True
        finally:
            try:
                if self.exception_caught == self._run_user(
                    self.case._run_teardown, self.result):
                    failed = True
            finally:
                try:
                    if self.exception_caught == self._run_user(
                        self._run_cleanups, self.result):
                        failed = True
                finally:
                    if not failed:
                        self.result.addSuccess(self.case,
                            details=self.case.getDetails())

    def _run_cleanups(self, result):
        """Run the cleanups that have been added with addCleanup.

        See the docstring for addCleanup for more information.

        :return: None if all cleanups ran without error,
            ``exception_caught`` if there was an error.
        """
        failing = False
        while self.case._cleanups:
            function, arguments, keywordArguments = self.case._cleanups.pop()
            got_exception = self._run_user(
                function, *arguments, **keywordArguments)
            if got_exception == self.exception_caught:
                failing = True
        if failing:
            return self.exception_caught

    def _run_user(self, fn, *args, **kwargs):
        """Run a user supplied function.

        Exceptions are processed by `_got_user_exception`.

        :return: Either whatever 'fn' returns or ``exception_caught`` if
            'fn' raised an exception.
        """
        try:
            return fn(*args, **kwargs)
        except KeyboardInterrupt:
            raise
        except:
            return self._got_user_exception(sys.exc_info())

    def _got_user_exception(self, exc_info, tb_label='traceback'):
        """Called when user code raises an exception.

        If 'exc_info' is a `MultipleExceptions`, then we recurse into it
        unpacking the errors that it's made up from.

        :param exc_info: A sys.exc_info() tuple for the user error.
        :param tb_label: An optional string label for the error.  If
            not specified, will default to 'traceback'.
        :return: 'exception_caught' if we catch one of the exceptions that
            have handlers in 'handlers', otherwise raise the error.
        """
        if exc_info[0] is MultipleExceptions:
            for sub_exc_info in exc_info[1].args:
                self._got_user_exception(sub_exc_info, tb_label)
            return self.exception_caught
        try:
            e = exc_info[1]
            self.case.onException(exc_info, tb_label=tb_label)
        finally:
            del exc_info
        for exc_class, handler in self.handlers:
            if isinstance(e, exc_class):
                self._exceptions.append(e)
                return self.exception_caught
        raise e


# Signal that this is part of the testing framework, and that code from this
# should not normally appear in tracebacks.
__unittest = True
