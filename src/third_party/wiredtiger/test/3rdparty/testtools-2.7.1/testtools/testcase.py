# Copyright (c) 2008-2011 testtools developers. See LICENSE for details.

"""Test case related stuff."""

__all__ = [
    'attr',
    'clone_test_with_new_id',
    'ExpectedException',
    'gather_details',
    'run_test_with',
    'skip',
    'skipIf',
    'skipUnless',
    'TestCase',
    'unique_text_generator',
    ]

import copy
import functools
import itertools
import sys
import types
import unittest
import warnings

from testtools.compat import reraise
from testtools import content
from testtools.helpers import try_import
from testtools.matchers import (
    Annotate,
    Contains,
    MatchesAll,
    MatchesException,
    MismatchError,
    Is,
    IsInstance,
    Not,
    Raises,
    )
from testtools.matchers._basic import _FlippedEquals
from testtools.monkey import patch
from testtools.runtest import (
    MultipleExceptions,
    RunTest,
)
from testtools.testresult import (
    ExtendedToOriginalDecorator,
    TestResult,
)


class TestSkipped(Exception):
    """Raised within TestCase.run() when a test is skipped."""
TestSkipped = try_import('unittest.case.SkipTest', TestSkipped)


class _UnexpectedSuccess(Exception):
    """An unexpected success was raised.

    Note that this exception is private plumbing in testtools' testcase
    module.
    """
_UnexpectedSuccess = try_import(
    'unittest.case._UnexpectedSuccess', _UnexpectedSuccess)


class _ExpectedFailure(Exception):
    """An expected failure occurred.

    Note that this exception is private plumbing in testtools' testcase
    module.
    """
_ExpectedFailure = try_import(
    'unittest.case._ExpectedFailure', _ExpectedFailure)


# Copied from unittest before python 3.4 release. Used to maintain
# compatibility with unittest sub-test feature. Users should not use this
# directly.
def _expectedFailure(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        try:
            func(*args, **kwargs)
        except Exception:
            raise _ExpectedFailure(sys.exc_info())
        raise _UnexpectedSuccess
    return wrapper


def run_test_with(test_runner, **kwargs):
    """Decorate a test as using a specific ``RunTest``.

    e.g.::

      @run_test_with(CustomRunner, timeout=42)
      def test_foo(self):
          self.assertTrue(True)

    The returned decorator works by setting an attribute on the decorated
    function.  `TestCase.__init__` looks for this attribute when deciding on a
    ``RunTest`` factory.  If you wish to use multiple decorators on a test
    method, then you must either make this one the top-most decorator, or you
    must write your decorators so that they update the wrapping function with
    the attributes of the wrapped function.  The latter is recommended style
    anyway.  ``functools.wraps``, ``functools.wrapper`` and
    ``twisted.python.util.mergeFunctionMetadata`` can help you do this.

    :param test_runner: A ``RunTest`` factory that takes a test case and an
        optional list of exception handlers.  See ``RunTest``.
    :param kwargs: Keyword arguments to pass on as extra arguments to
        'test_runner'.
    :return: A decorator to be used for marking a test as needing a special
        runner.
    """
    def decorator(function):
        # Set an attribute on 'function' which will inform TestCase how to
        # make the runner.
        def _run_test_with(case, handlers=None, last_resort=None):
            try:
                return test_runner(
                    case, handlers=handlers, last_resort=last_resort,
                    **kwargs)
            except TypeError:
                # Backwards compat: if we can't call the constructor
                # with last_resort, try without that.
                return test_runner(case, handlers=handlers, **kwargs)
        function._run_test_with = _run_test_with
        return function
    return decorator


def _copy_content(content_object):
    """Make a copy of the given content object.

    The content within ``content_object`` is iterated and saved. This is
    useful when the source of the content is volatile, a log file in a
    temporary directory for example.

    :param content_object: A `content.Content` instance.
    :return: A `content.Content` instance with the same mime-type as
        ``content_object`` and a non-volatile copy of its content.
    """
    content_bytes = list(content_object.iter_bytes())
    def content_callback():
        return content_bytes
    return content.Content(content_object.content_type, content_callback)


def gather_details(source_dict, target_dict):
    """Merge the details from ``source_dict`` into ``target_dict``.

    ``gather_details`` evaluates all details in ``source_dict``. Do not use it
    if the details are not ready to be evaluated.

    :param source_dict: A dictionary of details will be gathered.
    :param target_dict: A dictionary into which details will be gathered.
    """
    for name, content_object in source_dict.items():
        new_name = name
        disambiguator = itertools.count(1)
        while new_name in target_dict:
            new_name = '%s-%d' % (name, next(disambiguator))
        name = new_name
        target_dict[name] = _copy_content(content_object)


# Circular import: fixtures imports gather_details from here, we import
# fixtures, leading to gather_details not being available and fixtures being
# unable to import it.
fixtures = try_import('fixtures')


def _mods(i, mod):
    (q, r) = divmod(i, mod)
    while True:
        yield r
        if not q:
            break
        (q, r) = divmod(q, mod)


def _unique_text(base_cp, cp_range, index):
    s = ''
    for m in _mods(index, cp_range):
        s += chr(base_cp + m)
    return s


def unique_text_generator(prefix):
    """Generates unique text values.

    Generates text values that are unique. Use this when you need arbitrary
    text in your test, or as a helper for custom anonymous factory methods.

    :param prefix: The prefix for text.
    :return: text that looks like '<prefix>-<text_with_unicode>'.
    :rtype: str
    """
    # 0x1e00 is the start of a range of glyphs that are easy to see are
    # unicode since they've got circles and dots and other diacriticals.
    # 0x1eff is the end of the range of these diacritical chars.
    BASE_CP = 0x1e00
    CP_RANGE = 0x1f00 - BASE_CP
    index = 0
    while True:
        unique_text = _unique_text(BASE_CP, CP_RANGE, index)
        yield f'{prefix}-{unique_text}'
        index = index + 1


class TestCase(unittest.TestCase):
    """Extensions to the basic TestCase.

    :ivar exception_handlers: Exceptions to catch from setUp, runTest and
        tearDown. This list is able to be modified at any time and consists of
        (exception_class, handler(case, result, exception_value)) pairs.
    :ivar force_failure: Force testtools.RunTest to fail the test after the
        test has completed.
    :cvar run_tests_with: A factory to make the ``RunTest`` to run tests with.
        Defaults to ``RunTest``.  The factory is expected to take a test case
        and an optional list of exception handlers.
    """

    skipException = TestSkipped

    run_tests_with = RunTest

    def __init__(self, *args, **kwargs):
        """Construct a TestCase.

        :param testMethod: The name of the method to run.
        :keyword runTest: Optional class to use to execute the test. If not
            supplied ``RunTest`` is used. The instance to be used is created
            when run() is invoked, so will be fresh each time. Overrides
            ``TestCase.run_tests_with`` if given.
        """
        runTest = kwargs.pop('runTest', None)
        super().__init__(*args, **kwargs)
        self._reset()
        test_method = self._get_test_method()
        if runTest is None:
            runTest = getattr(
                test_method, '_run_test_with', self.run_tests_with)
        self.__RunTest = runTest
        if getattr(test_method, '__unittest_expecting_failure__', False):
            setattr(self, self._testMethodName, _expectedFailure(test_method))
        # Used internally for onException processing - used to gather extra
        # data from exceptions.
        self.__exception_handlers = []
        # Passed to RunTest to map exceptions to result actions
        self.exception_handlers = [
            (self.skipException, self._report_skip),
            (self.failureException, self._report_failure),
            (_ExpectedFailure, self._report_expected_failure),
            (_UnexpectedSuccess, self._report_unexpected_success),
            (Exception, self._report_error),
            ]

    def _reset(self):
        """Reset the test case as if it had never been run."""
        self._cleanups = []
        self._unique_id_gen = itertools.count(1)
        # Generators to ensure unique traceback ids.  Maps traceback label to
        # iterators.
        self._traceback_id_gens = {}
        self.__setup_called = False
        self.__teardown_called = False
        # __details is lazy-initialized so that a constructed-but-not-run
        # TestCase is safe to use with clone_test_with_new_id.
        self.__details = None

    def __eq__(self, other):
        eq = getattr(unittest.TestCase, '__eq__', None)
        if eq is not None and not unittest.TestCase.__eq__(self, other):
            return False
        return self.__dict__ == getattr(other, '__dict__', None)

    __hash__ = unittest.TestCase.__hash__

    def __repr__(self):
        # We add id to the repr because it makes testing testtools easier.
        return f"<{self.id()} id=0x{id(self):0x}>"

    def addDetail(self, name, content_object):
        """Add a detail to be reported with this test's outcome.

        For more details see pydoc testtools.TestResult.

        :param name: The name to give this detail.
        :param content_object: The content object for this detail. See
            testtools.content for more detail.
        """
        if self.__details is None:
            self.__details = {}
        self.__details[name] = content_object

    def getDetails(self):
        """Get the details dict that will be reported with this test's outcome.

        For more details see pydoc testtools.TestResult.
        """
        if self.__details is None:
            self.__details = {}
        return self.__details

    def patch(self, obj, attribute, value):
        """Monkey-patch 'obj.attribute' to 'value' while the test is running.

        If 'obj' has no attribute, then the monkey-patch will still go ahead,
        and the attribute will be deleted instead of restored to its original
        value.

        :param obj: The object to patch. Can be anything.
        :param attribute: The attribute on 'obj' to patch.
        :param value: The value to set 'obj.attribute' to.
        """
        self.addCleanup(patch(obj, attribute, value))

    def shortDescription(self):
        return self.id()

    def skipTest(self, reason):
        """Cause this test to be skipped.

        This raises self.skipException(reason). skipException is raised
        to permit a skip to be triggered at any point (during setUp or the
        testMethod itself). The run() method catches skipException and
        translates that into a call to the result objects addSkip method.

        :param reason: The reason why the test is being skipped. This must
            support being cast into a unicode string for reporting.
        """
        raise self.skipException(reason)

    def skip(self, reason):
        """DEPRECATED: Use skipTest instead."""
        warnings.warn(
            'Only valid in 1.8.1 and earlier. Use skipTest instead.',
            DeprecationWarning, stacklevel=2)
        self.skipTest(reason)

    def _formatTypes(self, classOrIterable):
        """Format a class or a bunch of classes for display in an error."""
        className = getattr(classOrIterable, '__name__', None)
        if className is None:
            className = ', '.join(klass.__name__ for klass in classOrIterable)
        return className

    def addCleanup(self, function, *arguments, **keywordArguments):
        """Add a cleanup function to be called after tearDown.

        Functions added with addCleanup will be called in reverse order of
        adding after tearDown, or after setUp if setUp raises an exception.

        If a function added with addCleanup raises an exception, the error
        will be recorded as a test error, and the next cleanup will then be
        run.

        Cleanup functions are always called before a test finishes running,
        even if setUp is aborted by an exception.
        """
        self._cleanups.append((function, arguments, keywordArguments))

    def addOnException(self, handler):
        """Add a handler to be called when an exception occurs in test code.

        This handler cannot affect what result methods are called, and is
        called before any outcome is called on the result object. An example
        use for it is to add some diagnostic state to the test details dict
        which is expensive to calculate and not interesting for reporting in
        the success case.

        Handlers are called before the outcome (such as addFailure) that
        the exception has caused.

        Handlers are called in first-added, first-called order, and if they
        raise an exception, that will propagate out of the test running
        machinery, halting test processing. As a result, do not call code that
        may unreasonably fail.
        """
        self.__exception_handlers.append(handler)

    def _add_reason(self, reason):
        self.addDetail('reason', content.text_content(reason))

    def assertEqual(self, expected, observed, message=''):
        """Assert that 'expected' is equal to 'observed'.

        :param expected: The expected value.
        :param observed: The observed value.
        :param message: An optional message to include in the error.
        """
        matcher = _FlippedEquals(expected)
        self.assertThat(observed, matcher, message)

    failUnlessEqual = assertEquals = assertEqual

    def assertIn(self, needle, haystack, message=''):
        """Assert that needle is in haystack."""
        self.assertThat(haystack, Contains(needle), message)

    def assertIsNone(self, observed, message=''):
        """Assert that 'observed' is equal to None.

        :param observed: The observed value.
        :param message: An optional message describing the error.
        """
        matcher = Is(None)
        self.assertThat(observed, matcher, message)

    def assertIsNotNone(self, observed, message=''):
        """Assert that 'observed' is not equal to None.

        :param observed: The observed value.
        :param message: An optional message describing the error.
        """
        matcher = Not(Is(None))
        self.assertThat(observed, matcher, message)

    def assertIs(self, expected, observed, message=''):
        """Assert that 'expected' is 'observed'.

        :param expected: The expected value.
        :param observed: The observed value.
        :param message: An optional message describing the error.
        """
        matcher = Is(expected)
        self.assertThat(observed, matcher, message)

    def assertIsNot(self, expected, observed, message=''):
        """Assert that 'expected' is not 'observed'."""
        matcher = Not(Is(expected))
        self.assertThat(observed, matcher, message)

    def assertNotIn(self, needle, haystack, message=''):
        """Assert that needle is not in haystack."""
        matcher = Not(Contains(needle))
        self.assertThat(haystack, matcher, message)

    def assertIsInstance(self, obj, klass, msg=None):
        if isinstance(klass, tuple):
            matcher = IsInstance(*klass)
        else:
            matcher = IsInstance(klass)
        self.assertThat(obj, matcher, msg)

    def assertRaises(self, excClass, callableObj, *args, **kwargs):
        """Fail unless an exception of class excClass is thrown
           by callableObj when invoked with arguments args and keyword
           arguments kwargs. If a different type of exception is
           thrown, it will not be caught, and the test case will be
           deemed to have suffered an error, exactly as for an
           unexpected exception.
        """
        class ReRaiseOtherTypes:
            def match(self, matchee):
                if not issubclass(matchee[0], excClass):
                    reraise(*matchee)

        class CaptureMatchee:
            def match(self, matchee):
                self.matchee = matchee[1]
        capture = CaptureMatchee()
        matcher = Raises(
            MatchesAll(ReRaiseOtherTypes(),
                       MatchesException(excClass), capture))
        our_callable = Nullary(callableObj, *args, **kwargs)
        self.assertThat(our_callable, matcher)
        return capture.matchee
    failUnlessRaises = assertRaises

    def assertThat(self, matchee, matcher, message='', verbose=False):
        """Assert that matchee is matched by matcher.

        :param matchee: An object to match with matcher.
        :param matcher: An object meeting the testtools.Matcher protocol.
        :raises MismatchError: When matcher does not match thing.
        """
        mismatch_error = self._matchHelper(matchee, matcher, message, verbose)
        if mismatch_error is not None:
            raise mismatch_error

    assertItemsEqual = unittest.TestCase.assertCountEqual
    def addDetailUniqueName(self, name, content_object):
        """Add a detail to the test, but ensure it's name is unique.

        This method checks whether ``name`` conflicts with a detail that has
        already been added to the test. If it does, it will modify ``name`` to
        avoid the conflict.

        For more details see pydoc testtools.TestResult.

        :param name: The name to give this detail.
        :param content_object: The content object for this detail. See
            testtools.content for more detail.
        """
        existing_details = self.getDetails()
        full_name = name
        suffix = 1
        while full_name in existing_details:
            full_name = "%s-%d" % (name, suffix)
            suffix += 1
        self.addDetail(full_name, content_object)

    def expectThat(self, matchee, matcher, message='', verbose=False):
        """Check that matchee is matched by matcher, but delay the assertion failure.

        This method behaves similarly to ``assertThat``, except that a failed
        match does not exit the test immediately. The rest of the test code
        will continue to run, and the test will be marked as failing after the
        test has finished.

        :param matchee: An object to match with matcher.
        :param matcher: An object meeting the testtools.Matcher protocol.
        :param message: If specified, show this message with any failed match.

        """
        mismatch_error = self._matchHelper(matchee, matcher, message, verbose)

        if mismatch_error is not None:
            self.addDetailUniqueName(
                "Failed expectation",
                content.StacktraceContent(
                    postfix_content="MismatchError: " + str(mismatch_error)
                )
            )
            self.force_failure = True

    def _matchHelper(self, matchee, matcher, message, verbose):
        matcher = Annotate.if_message(message, matcher)
        mismatch = matcher.match(matchee)
        if not mismatch:
            return
        for (name, value) in mismatch.get_details().items():
            self.addDetailUniqueName(name, value)
        return MismatchError(matchee, matcher, mismatch, verbose)

    def defaultTestResult(self):
        return TestResult()

    def expectFailure(self, reason, predicate, *args, **kwargs):
        """Check that a test fails in a particular way.

        If the test fails in the expected way, a KnownFailure is caused. If it
        succeeds an UnexpectedSuccess is caused.

        The expected use of expectFailure is as a barrier at the point in a
        test where the test would fail. For example:
        >>> def test_foo(self):
        >>>    self.expectFailure("1 should be 0", self.assertNotEqual, 1, 0)
        >>>    self.assertEqual(1, 0)

        If in the future 1 were to equal 0, the expectFailure call can simply
        be removed. This separation preserves the original intent of the test
        while it is in the expectFailure mode.
        """
        # TODO: implement with matchers.
        self._add_reason(reason)
        try:
            predicate(*args, **kwargs)
        except self.failureException:
            # GZ 2010-08-12: Don't know how to avoid exc_info cycle as the new
            #                unittest _ExpectedFailure wants old traceback
            exc_info = sys.exc_info()
            try:
                self._report_traceback(exc_info)
                raise _ExpectedFailure(exc_info)
            finally:
                del exc_info
        else:
            raise _UnexpectedSuccess(reason)

    def getUniqueInteger(self):
        """Get an integer unique to this test.

        Returns an integer that is guaranteed to be unique to this instance.
        Use this when you need an arbitrary integer in your test, or as a
        helper for custom anonymous factory methods.
        """
        return next(self._unique_id_gen)

    def getUniqueString(self, prefix=None):
        """Get a string unique to this test.

        Returns a string that is guaranteed to be unique to this instance. Use
        this when you need an arbitrary string in your test, or as a helper
        for custom anonymous factory methods.

        :param prefix: The prefix of the string. If not provided, defaults
            to the id of the tests.
        :return: A bytestring of '<prefix>-<unique_int>'.
        """
        if prefix is None:
            prefix = self.id()
        return '%s-%d' % (prefix, self.getUniqueInteger())

    def onException(self, exc_info, tb_label='traceback'):
        """Called when an exception propagates from test code.

        :seealso addOnException:
        """
        if exc_info[0] not in [
                self.skipException, _UnexpectedSuccess, _ExpectedFailure]:
            self._report_traceback(exc_info, tb_label=tb_label)
        for handler in self.__exception_handlers:
            handler(exc_info)

    @staticmethod
    def _report_error(self, result, err):
        result.addError(self, details=self.getDetails())

    @staticmethod
    def _report_expected_failure(self, result, err):
        result.addExpectedFailure(self, details=self.getDetails())

    @staticmethod
    def _report_failure(self, result, err):
        result.addFailure(self, details=self.getDetails())

    @staticmethod
    def _report_skip(self, result, err):
        if err.args:
            reason = err.args[0]
        else:
            reason = "no reason given."
        self._add_reason(reason)
        result.addSkip(self, details=self.getDetails())

    def _report_traceback(self, exc_info, tb_label='traceback'):
        id_gen = self._traceback_id_gens.setdefault(
            tb_label, itertools.count(0))
        while True:
            tb_id = next(id_gen)
            if tb_id:
                tb_label = '%s-%d' % (tb_label, tb_id)
            if tb_label not in self.getDetails():
                break
        self.addDetail(tb_label, content.TracebackContent(
            exc_info, self, capture_locals=getattr(
                self, '__testtools_tb_locals__', False)))

    @staticmethod
    def _report_unexpected_success(self, result, err):
        result.addUnexpectedSuccess(self, details=self.getDetails())

    def run(self, result=None):
        self._reset()
        try:
            run_test = self.__RunTest(
                self, self.exception_handlers, last_resort=self._report_error)
        except TypeError:
            # Backwards compat: if we can't call the constructor
            # with last_resort, try without that.
            run_test = self.__RunTest(self, self.exception_handlers)
        return run_test.run(result)

    def _run_setup(self, result):
        """Run the setUp function for this test.

        :param result: A testtools.TestResult to report activity to.
        :raises ValueError: If the base class setUp is not called, a
            ValueError is raised.
        """
        ret = self.setUp()
        if not self.__setup_called:
            raise ValueError(
                "In File: %s\n"
                "TestCase.setUp was not called. Have you upcalled all the "
                "way up the hierarchy from your setUp? e.g. Call "
                "super(%s, self).setUp() from your setUp()."
                % (sys.modules[self.__class__.__module__].__file__,
                   self.__class__.__name__))
        return ret

    def _run_teardown(self, result):
        """Run the tearDown function for this test.

        :param result: A testtools.TestResult to report activity to.
        :raises ValueError: If the base class tearDown is not called, a
            ValueError is raised.
        """
        ret = self.tearDown()
        if not self.__teardown_called:
            raise ValueError(
                "In File: %s\n"
                "TestCase.tearDown was not called. Have you upcalled all the "
                "way up the hierarchy from your tearDown? e.g. Call "
                "super(%s, self).tearDown() from your tearDown()."
                % (sys.modules[self.__class__.__module__].__file__,
                   self.__class__.__name__))
        return ret

    def _get_test_method(self):
        method_name = getattr(self, '_testMethodName')
        return getattr(self, method_name)

    def _run_test_method(self, result):
        """Run the test method for this test.

        :param result: A testtools.TestResult to report activity to.
        :return: None.
        """
        return self._get_test_method()()

    def useFixture(self, fixture):
        """Use fixture in a test case.

        The fixture will be setUp, and self.addCleanup(fixture.cleanUp) called.

        :param fixture: The fixture to use.
        :return: The fixture, after setting it up and scheduling a cleanup for
           it.
        """
        try:
            fixture.setUp()
        except MultipleExceptions as e:
            if (fixtures is not None and
                    e.args[-1][0] is fixtures.fixture.SetupError):
                gather_details(e.args[-1][1].args[0], self.getDetails())
            raise
        except:
            exc_info = sys.exc_info()
            try:
                # fixture._details is not available if using the newer
                # _setUp() API in Fixtures because it already cleaned up
                # the fixture.  Ideally this whole try/except is not
                # really needed any more, however, we keep this code to
                # remain compatible with the older setUp().
                if (
                    hasattr(fixture, '_details') and
                    fixture._details is not None
                ):
                    gather_details(fixture.getDetails(), self.getDetails())
            except:
                # Report the setUp exception, then raise the error during
                # gather_details.
                self._report_traceback(exc_info)
                raise
            else:
                # Gather_details worked, so raise the exception setUp
                # encountered.
                reraise(*exc_info)
        else:
            self.addCleanup(fixture.cleanUp)
            self.addCleanup(
                gather_details, fixture.getDetails(), self.getDetails())
            return fixture

    def setUp(self):
        super().setUp()
        if self.__setup_called:
            raise ValueError(
                "In File: %s\n"
                "TestCase.setUp was already called. Do not explicitly call "
                "setUp from your tests. In your own setUp, use super to call "
                "the base setUp."
                % (sys.modules[self.__class__.__module__].__file__,))
        self.__setup_called = True

    def tearDown(self):
        super().tearDown()
        if self.__teardown_called:
            raise ValueError(
                "In File: %s\n"
                "TestCase.tearDown was already called. Do not explicitly call "
                "tearDown from your tests. In your own tearDown, use super to "
                "call the base tearDown."
                % (sys.modules[self.__class__.__module__].__file__,))
        self.__teardown_called = True


class PlaceHolder:
    """A placeholder test.

    `PlaceHolder` implements much of the same interface as TestCase and is
    particularly suitable for being added to TestResults.
    """

    failureException = None

    def __init__(self, test_id, short_description=None, details=None,
        outcome='addSuccess', error=None, tags=None, timestamps=(None, None)):
        """Construct a `PlaceHolder`.

        :param test_id: The id of the placeholder test.
        :param short_description: The short description of the place holder
            test. If not provided, the id will be used instead.
        :param details: Outcome details as accepted by addSuccess etc.
        :param outcome: The outcome to call. Defaults to 'addSuccess'.
        :param tags: Tags to report for the test.
        :param timestamps: A two-tuple of timestamps for the test start and
            finish. Each timestamp may be None to indicate it is not known.
        """
        self._test_id = test_id
        self._short_description = short_description
        self._details = details or {}
        self._outcome = outcome
        if error is not None:
            self._details['traceback'] = content.TracebackContent(error, self)
        tags = tags or frozenset()
        self._tags = frozenset(tags)
        self._timestamps = timestamps

    def __call__(self, result=None):
        return self.run(result=result)

    def __repr__(self):
        internal = [self._outcome, self._test_id, self._details]
        if self._short_description is not None:
            internal.append(self._short_description)
        return "<{}.{}({})>".format(
            self.__class__.__module__,
            self.__class__.__name__,
            ", ".join(map(repr, internal)))

    def __str__(self):
        return self.id()

    def countTestCases(self):
        return 1

    def debug(self):
        pass

    def id(self):
        return self._test_id

    def _result(self, result):
        if result is None:
            return TestResult()
        else:
            return ExtendedToOriginalDecorator(result)

    def run(self, result=None):
        result = self._result(result)
        if self._timestamps[0] is not None:
            result.time(self._timestamps[0])
        result.tags(self._tags, set())
        result.startTest(self)
        if self._timestamps[1] is not None:
            result.time(self._timestamps[1])
        outcome = getattr(result, self._outcome)
        outcome(self, details=self._details)
        result.stopTest(self)
        result.tags(set(), self._tags)

    def shortDescription(self):
        if self._short_description is None:
            return self.id()
        else:
            return self._short_description


def ErrorHolder(test_id, error, short_description=None, details=None):
    """Construct an `ErrorHolder`.

    :param test_id: The id of the test.
    :param error: The exc info tuple that will be used as the test's error.
        This is inserted into the details as 'traceback' - any existing key
        will be overridden.
    :param short_description: An optional short description of the test.
    :param details: Outcome details as accepted by addSuccess etc.
    """
    return PlaceHolder(
        test_id, short_description=short_description,
        details=details, outcome='addError', error=error)


def _clone_test_id_callback(test, callback):
    """Copy a `TestCase`, and make it call callback for its id().

    This is only expected to be used on tests that have been constructed but
    not executed.

    :param test: A TestCase instance.
    :param callback: A callable that takes no parameters and returns a string.
    :return: A copy.copy of the test with id=callback.
    """
    newTest = copy.copy(test)
    newTest.id = callback
    return newTest


def clone_test_with_new_id(test, new_id):
    """Copy a `TestCase`, and give the copied test a new id.

    This is only expected to be used on tests that have been constructed but
    not executed.
    """
    return _clone_test_id_callback(test, lambda: new_id)


def attr(*args):
    """Decorator for adding attributes to WithAttributes.

    :param args: The name of attributes to add.
    :return: A callable that when applied to a WithAttributes will
        alter its id to enumerate the added attributes.
    """
    def decorate(fn):
        if not hasattr(fn, '__testtools_attrs'):
            fn.__testtools_attrs = set()
        fn.__testtools_attrs.update(args)
        return fn
    return decorate


class WithAttributes:
    """A mix-in class for modifying test id by attributes.

    e.g.
    >>> class MyTest(WithAttributes, TestCase):
    ...    @attr('foo')
    ...    def test_bar(self):
    ...        pass
    >>> MyTest('test_bar').id()
    testtools.testcase.MyTest/test_bar[foo]
    """

    def id(self):
        orig = super().id()
        # Depends on testtools.TestCase._get_test_method, be nice to support
        # plain unittest.
        fn = self._get_test_method()
        attributes = getattr(fn, '__testtools_attrs', None)
        if not attributes:
            return orig
        return orig + '[' + ','.join(sorted(attributes)) + ']'


class_types = [type]
if getattr(types, 'ClassType', None) is not None:
    class_types.append(types.ClassType)
class_types = tuple(class_types)


def skip(reason):
    """A decorator to skip unit tests.

    This is just syntactic sugar so users don't have to change any of their
    unit tests in order to migrate to python 2.7, which provides the
    @unittest.skip decorator.
    """
    def decorator(test_item):
        if not isinstance(test_item, class_types):
            @functools.wraps(test_item)
            def skip_wrapper(*args, **kwargs):
                raise TestCase.skipException(reason)
            test_item = skip_wrapper

        # This attribute signals to RunTest._run_core that the entire test
        # must be skipped - including setUp and tearDown. This makes us
        # compatible with testtools.skip* functions, which set the same
        # attributes.
        test_item.__unittest_skip__ = True
        test_item.__unittest_skip_why__ = reason
        return test_item
    return decorator


def skipIf(condition, reason):
    """A decorator to skip a test if the condition is true."""
    if condition:
        return skip(reason)

    def _id(obj):
        return obj
    return _id


def skipUnless(condition, reason):
    """A decorator to skip a test unless the condition is true."""
    if not condition:
        return skip(reason)

    def _id(obj):
        return obj
    return _id


class ExpectedException:
    """A context manager to handle expected exceptions.

      def test_foo(self):
          with ExpectedException(ValueError, 'fo.*'):
              raise ValueError('foo')

    will pass.  If the raised exception has a type other than the specified
    type, it will be re-raised.  If it has a 'str()' that does not match the
    given regular expression, an AssertionError will be raised.  If no
    exception is raised, an AssertionError will be raised.
    """

    def __init__(self, exc_type, value_re=None, msg=None):
        """Construct an `ExpectedException`.

        :param exc_type: The type of exception to expect.
        :param value_re: A regular expression to match against the
            'str()' of the raised exception.
        :param msg: An optional message explaining the failure.
        """
        self.exc_type = exc_type
        self.value_re = value_re
        self.msg = msg

    def __enter__(self):
        pass

    def __exit__(self, exc_type, exc_value, traceback):
        if exc_type is None:
            error_msg = '%s not raised.' % self.exc_type.__name__
            if self.msg:
                error_msg = error_msg + ' : ' + self.msg
            raise AssertionError(error_msg)
        if exc_type != self.exc_type:
            return False
        if self.value_re:
            matcher = MatchesException(self.exc_type, self.value_re)
            if self.msg:
                matcher = Annotate(self.msg, matcher)
            mismatch = matcher.match((exc_type, exc_value, traceback))
            if mismatch:
                raise AssertionError(mismatch.describe())
        return True


class Nullary:
    """Turn a callable into a nullary callable.

    The advantage of this over ``lambda: f(*args, **kwargs)`` is that it
    preserves the ``repr()`` of ``f``.
    """

    def __init__(self, callable_object, *args, **kwargs):
        self._callable_object = callable_object
        self._args = args
        self._kwargs = kwargs

    def __call__(self):
        return self._callable_object(*self._args, **self._kwargs)

    def __repr__(self):
        return repr(self._callable_object)


class DecorateTestCaseResult:
    """Decorate a TestCase and permit customisation of the result for runs."""

    def __init__(self, case, callout, before_run=None, after_run=None):
        """Construct a DecorateTestCaseResult.

        :param case: The case to decorate.
        :param callout: A callback to call when run/__call__/debug is called.
            Must take a result parameter and return a result object to be used.
            For instance: lambda result: result.
        :param before_run: If set, call this with the decorated result before
            calling into the decorated run/__call__ method.
        :param before_run: If set, call this with the decorated result after
            calling into the decorated run/__call__ method.
        """
        self.decorated = case
        self.callout = callout
        self.before_run = before_run
        self.after_run = after_run

    def _run(self, result, run_method):
        result = self.callout(result)
        if self.before_run:
            self.before_run(result)
        try:
            return run_method(result)
        finally:
            if self.after_run:
                self.after_run(result)

    def run(self, result=None):
        self._run(result, self.decorated.run)

    def __call__(self, result=None):
        self._run(result, self.decorated)

    def __getattr__(self, name):
        return getattr(self.decorated, name)

    def __delattr__(self, name):
        delattr(self.decorated, name)

    def __setattr__(self, name, value):
        if name in ('decorated', 'callout', 'before_run', 'after_run'):
            self.__dict__[name] = value
            return
        setattr(self.decorated, name, value)


# Signal that this is part of the testing framework, and that code from this
# should not normally appear in tracebacks.
__unittest = True
