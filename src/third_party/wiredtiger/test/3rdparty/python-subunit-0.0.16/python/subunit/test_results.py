#
#  subunit: extensions to Python unittest to get test results from subprocesses.
#  Copyright (C) 2009  Robert Collins <robertc@robertcollins.net>
#
#  Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
#  license at the users choice. A copy of both licenses are available in the
#  project source as Apache-2.0 and BSD. You may not use this file except in
#  compliance with one of these two licences.
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
#  license you chose for the specific language governing permissions and
#  limitations under that license.
#

"""TestResult helper classes used to by subunit."""

import csv
import datetime

import testtools
from testtools.compat import all
from testtools.content import (
    text_content,
    TracebackContent,
    )
from testtools import StreamResult

from subunit import iso8601
import subunit


# NOT a TestResult, because we are implementing the interface, not inheriting
# it.
class TestResultDecorator(object):
    """General pass-through decorator.

    This provides a base that other TestResults can inherit from to
    gain basic forwarding functionality. It also takes care of
    handling the case where the target doesn't support newer methods
    or features by degrading them.
    """

    # XXX: Since lp:testtools r250, this is in testtools. Once it's released,
    # we should gut this and just use that.

    def __init__(self, decorated):
        """Create a TestResultDecorator forwarding to decorated."""
        # Make every decorator degrade gracefully.
        self.decorated = testtools.ExtendedToOriginalDecorator(decorated)

    def startTest(self, test):
        return self.decorated.startTest(test)

    def startTestRun(self):
        return self.decorated.startTestRun()

    def stopTest(self, test):
        return self.decorated.stopTest(test)

    def stopTestRun(self):
        return self.decorated.stopTestRun()

    def addError(self, test, err=None, details=None):
        return self.decorated.addError(test, err, details=details)

    def addFailure(self, test, err=None, details=None):
        return self.decorated.addFailure(test, err, details=details)

    def addSuccess(self, test, details=None):
        return self.decorated.addSuccess(test, details=details)

    def addSkip(self, test, reason=None, details=None):
        return self.decorated.addSkip(test, reason, details=details)

    def addExpectedFailure(self, test, err=None, details=None):
        return self.decorated.addExpectedFailure(test, err, details=details)

    def addUnexpectedSuccess(self, test, details=None):
        return self.decorated.addUnexpectedSuccess(test, details=details)

    def _get_failfast(self):
        return getattr(self.decorated, 'failfast', False)

    def _set_failfast(self, value):
        self.decorated.failfast = value
    failfast = property(_get_failfast, _set_failfast)

    def progress(self, offset, whence):
        return self.decorated.progress(offset, whence)

    def wasSuccessful(self):
        return self.decorated.wasSuccessful()

    @property
    def shouldStop(self):
        return self.decorated.shouldStop

    def stop(self):
        return self.decorated.stop()

    @property
    def testsRun(self):
        return self.decorated.testsRun

    def tags(self, new_tags, gone_tags):
        return self.decorated.tags(new_tags, gone_tags)

    def time(self, a_datetime):
        return self.decorated.time(a_datetime)


class HookedTestResultDecorator(TestResultDecorator):
    """A TestResult which calls a hook on every event."""

    def __init__(self, decorated):
        self.super = super(HookedTestResultDecorator, self)
        self.super.__init__(decorated)

    def startTest(self, test):
        self._before_event()
        return self.super.startTest(test)

    def startTestRun(self):
        self._before_event()
        return self.super.startTestRun()

    def stopTest(self, test):
        self._before_event()
        return self.super.stopTest(test)

    def stopTestRun(self):
        self._before_event()
        return self.super.stopTestRun()

    def addError(self, test, err=None, details=None):
        self._before_event()
        return self.super.addError(test, err, details=details)

    def addFailure(self, test, err=None, details=None):
        self._before_event()
        return self.super.addFailure(test, err, details=details)

    def addSuccess(self, test, details=None):
        self._before_event()
        return self.super.addSuccess(test, details=details)

    def addSkip(self, test, reason=None, details=None):
        self._before_event()
        return self.super.addSkip(test, reason, details=details)

    def addExpectedFailure(self, test, err=None, details=None):
        self._before_event()
        return self.super.addExpectedFailure(test, err, details=details)

    def addUnexpectedSuccess(self, test, details=None):
        self._before_event()
        return self.super.addUnexpectedSuccess(test, details=details)

    def progress(self, offset, whence):
        self._before_event()
        return self.super.progress(offset, whence)

    def wasSuccessful(self):
        self._before_event()
        return self.super.wasSuccessful()

    @property
    def shouldStop(self):
        self._before_event()
        return self.super.shouldStop

    def stop(self):
        self._before_event()
        return self.super.stop()

    def time(self, a_datetime):
        self._before_event()
        return self.super.time(a_datetime)


class AutoTimingTestResultDecorator(HookedTestResultDecorator):
    """Decorate a TestResult to add time events to a test run.

    By default this will cause a time event before every test event,
    but if explicit time data is being provided by the test run, then
    this decorator will turn itself off to prevent causing confusion.
    """

    def __init__(self, decorated):
        self._time = None
        super(AutoTimingTestResultDecorator, self).__init__(decorated)

    def _before_event(self):
        time = self._time
        if time is not None:
            return
        time = datetime.datetime.utcnow().replace(tzinfo=iso8601.Utc())
        self.decorated.time(time)

    def progress(self, offset, whence):
        return self.decorated.progress(offset, whence)

    @property
    def shouldStop(self):
        return self.decorated.shouldStop

    def time(self, a_datetime):
        """Provide a timestamp for the current test activity.

        :param a_datetime: If None, automatically add timestamps before every
            event (this is the default behaviour if time() is not called at
            all).  If not None, pass the provided time onto the decorated
            result object and disable automatic timestamps.
        """
        self._time = a_datetime
        return self.decorated.time(a_datetime)


class TagsMixin(object):

    def __init__(self):
        self._clear_tags()

    def _clear_tags(self):
        self._global_tags = set(), set()
        self._test_tags = None

    def _get_active_tags(self):
        global_new, global_gone = self._global_tags
        if self._test_tags is None:
            return set(global_new)
        test_new, test_gone = self._test_tags
        return global_new.difference(test_gone).union(test_new)

    def _get_current_scope(self):
        if self._test_tags:
            return self._test_tags
        return self._global_tags

    def _flush_current_scope(self, tag_receiver):
        new_tags, gone_tags = self._get_current_scope()
        if new_tags or gone_tags:
            tag_receiver.tags(new_tags, gone_tags)
        if self._test_tags:
            self._test_tags = set(), set()
        else:
            self._global_tags = set(), set()

    def startTestRun(self):
        self._clear_tags()

    def startTest(self, test):
        self._test_tags = set(), set()

    def stopTest(self, test):
        self._test_tags = None

    def tags(self, new_tags, gone_tags):
        """Handle tag instructions.

        Adds and removes tags as appropriate. If a test is currently running,
        tags are not affected for subsequent tests.

        :param new_tags: Tags to add,
        :param gone_tags: Tags to remove.
        """
        current_new_tags, current_gone_tags = self._get_current_scope()
        current_new_tags.update(new_tags)
        current_new_tags.difference_update(gone_tags)
        current_gone_tags.update(gone_tags)
        current_gone_tags.difference_update(new_tags)


class TagCollapsingDecorator(HookedTestResultDecorator, TagsMixin):
    """Collapses many 'tags' calls into one where possible."""

    def __init__(self, result):
        super(TagCollapsingDecorator, self).__init__(result)
        self._clear_tags()

    def _before_event(self):
        self._flush_current_scope(self.decorated)

    def tags(self, new_tags, gone_tags):
        TagsMixin.tags(self, new_tags, gone_tags)


class TimeCollapsingDecorator(HookedTestResultDecorator):
    """Only pass on the first and last of a consecutive sequence of times."""

    def __init__(self, decorated):
        super(TimeCollapsingDecorator, self).__init__(decorated)
        self._last_received_time = None
        self._last_sent_time = None

    def _before_event(self):
        if self._last_received_time is None:
            return
        if self._last_received_time != self._last_sent_time:
            self.decorated.time(self._last_received_time)
            self._last_sent_time = self._last_received_time
        self._last_received_time = None

    def time(self, a_time):
        # Don't upcall, because we don't want to call _before_event, it's only
        # for non-time events.
        if self._last_received_time is None:
            self.decorated.time(a_time)
            self._last_sent_time = a_time
        self._last_received_time = a_time


def and_predicates(predicates):
    """Return a predicate that is true iff all predicates are true."""
    # XXX: Should probably be in testtools to be better used by matchers. jml
    return lambda *args, **kwargs: all(p(*args, **kwargs) for p in predicates)


def make_tag_filter(with_tags, without_tags):
    """Make a callback that checks tests against tags."""

    with_tags = with_tags and set(with_tags) or None
    without_tags = without_tags and set(without_tags) or None

    def check_tags(test, outcome, err, details, tags):
        if with_tags and not with_tags <= tags:
            return False
        if without_tags and bool(without_tags & tags):
            return False
        return True

    return check_tags


class _PredicateFilter(TestResultDecorator, TagsMixin):

    def __init__(self, result, predicate):
        super(_PredicateFilter, self).__init__(result)
        self._clear_tags()
        self.decorated = TimeCollapsingDecorator(
            TagCollapsingDecorator(self.decorated))
        self._predicate = predicate
        # The current test (for filtering tags)
        self._current_test = None
        # Has the current test been filtered (for outputting test tags)
        self._current_test_filtered = None
        # Calls to this result that we don't know whether to forward on yet.
        self._buffered_calls = []

    def filter_predicate(self, test, outcome, error, details):
        return self._predicate(
            test, outcome, error, details, self._get_active_tags())

    def addError(self, test, err=None, details=None):
        if (self.filter_predicate(test, 'error', err, details)):
            self._buffered_calls.append(
                ('addError', [test, err], {'details': details}))
        else:
            self._filtered()

    def addFailure(self, test, err=None, details=None):
        if (self.filter_predicate(test, 'failure', err, details)):
            self._buffered_calls.append(
                ('addFailure', [test, err], {'details': details}))
        else:
            self._filtered()

    def addSkip(self, test, reason=None, details=None):
        if (self.filter_predicate(test, 'skip', reason, details)):
            self._buffered_calls.append(
                ('addSkip', [test, reason], {'details': details}))
        else:
            self._filtered()

    def addExpectedFailure(self, test, err=None, details=None):
        if self.filter_predicate(test, 'expectedfailure', err, details):
            self._buffered_calls.append(
                ('addExpectedFailure', [test, err], {'details': details}))
        else:
            self._filtered()

    def addUnexpectedSuccess(self, test, details=None):
        self._buffered_calls.append(
            ('addUnexpectedSuccess', [test], {'details': details}))

    def addSuccess(self, test, details=None):
        if (self.filter_predicate(test, 'success', None, details)):
            self._buffered_calls.append(
                ('addSuccess', [test], {'details': details}))
        else:
            self._filtered()

    def _filtered(self):
        self._current_test_filtered = True

    def startTest(self, test):
        """Start a test.

        Not directly passed to the client, but used for handling of tags
        correctly.
        """
        TagsMixin.startTest(self, test)
        self._current_test = test
        self._current_test_filtered = False
        self._buffered_calls.append(('startTest', [test], {}))

    def stopTest(self, test):
        """Stop a test.

        Not directly passed to the client, but used for handling of tags
        correctly.
        """
        if not self._current_test_filtered:
            for method, args, kwargs in self._buffered_calls:
                getattr(self.decorated, method)(*args, **kwargs)
            self.decorated.stopTest(test)
        self._current_test = None
        self._current_test_filtered = None
        self._buffered_calls = []
        TagsMixin.stopTest(self, test)

    def tags(self, new_tags, gone_tags):
        TagsMixin.tags(self, new_tags, gone_tags)
        if self._current_test is not None:
            self._buffered_calls.append(('tags', [new_tags, gone_tags], {}))
        else:
            return super(_PredicateFilter, self).tags(new_tags, gone_tags)

    def time(self, a_time):
        return self.decorated.time(a_time)

    def id_to_orig_id(self, id):
        if id.startswith("subunit.RemotedTestCase."):
            return id[len("subunit.RemotedTestCase."):]
        return id


class TestResultFilter(TestResultDecorator):
    """A pyunit TestResult interface implementation which filters tests.

    Tests that pass the filter are handed on to another TestResult instance
    for further processing/reporting. To obtain the filtered results,
    the other instance must be interrogated.

    :ivar result: The result that tests are passed to after filtering.
    :ivar filter_predicate: The callback run to decide whether to pass
        a result.
    """

    def __init__(self, result, filter_error=False, filter_failure=False,
        filter_success=True, filter_skip=False, filter_xfail=False,
        filter_predicate=None, fixup_expected_failures=None):
        """Create a FilterResult object filtering to result.

        :param filter_error: Filter out errors.
        :param filter_failure: Filter out failures.
        :param filter_success: Filter out successful tests.
        :param filter_skip: Filter out skipped tests.
        :param filter_xfail: Filter out expected failure tests.
        :param filter_predicate: A callable taking (test, outcome, err,
            details, tags) and returning True if the result should be passed
            through.  err and details may be none if no error or extra
            metadata is available. outcome is the name of the outcome such
            as 'success' or 'failure'. tags is new in 0.0.8; 0.0.7 filters
            are still supported but should be updated to accept the tags
            parameter for efficiency.
        :param fixup_expected_failures: Set of test ids to consider known
            failing.
        """
        predicates = []
        if filter_error:
            predicates.append(
                lambda t, outcome, e, d, tags: outcome != 'error')
        if filter_failure:
            predicates.append(
                lambda t, outcome, e, d, tags: outcome != 'failure')
        if filter_success:
            predicates.append(
                lambda t, outcome, e, d, tags: outcome != 'success')
        if filter_skip:
            predicates.append(
                lambda t, outcome, e, d, tags: outcome != 'skip')
        if filter_xfail:
            predicates.append(
                lambda t, outcome, e, d, tags: outcome != 'expectedfailure')
        if filter_predicate is not None:
            def compat(test, outcome, error, details, tags):
                # 0.0.7 and earlier did not support the 'tags' parameter.
                try:
                    return filter_predicate(
                        test, outcome, error, details, tags)
                except TypeError:
                    return filter_predicate(test, outcome, error, details)
            predicates.append(compat)
        predicate = and_predicates(predicates)
        super(TestResultFilter, self).__init__(
            _PredicateFilter(result, predicate))
        if fixup_expected_failures is None:
            self._fixup_expected_failures = frozenset()
        else:
            self._fixup_expected_failures = fixup_expected_failures

    def addError(self, test, err=None, details=None):
        if self._failure_expected(test):
            self.addExpectedFailure(test, err=err, details=details)
        else:
            super(TestResultFilter, self).addError(
                test, err=err, details=details)

    def addFailure(self, test, err=None, details=None):
        if self._failure_expected(test):
            self.addExpectedFailure(test, err=err, details=details)
        else:
            super(TestResultFilter, self).addFailure(
                test, err=err, details=details)

    def addSuccess(self, test, details=None):
        if self._failure_expected(test):
            self.addUnexpectedSuccess(test, details=details)
        else:
            super(TestResultFilter, self).addSuccess(test, details=details)

    def _failure_expected(self, test):
        return (test.id() in self._fixup_expected_failures)


class TestIdPrintingResult(testtools.TestResult):
    """Print test ids to a stream.

    Implements both TestResult and StreamResult, for compatibility.
    """

    def __init__(self, stream, show_times=False, show_exists=False):
        """Create a FilterResult object outputting to stream."""
        super(TestIdPrintingResult, self).__init__()
        self._stream = stream
        self.show_exists = show_exists
        self.show_times = show_times

    def startTestRun(self):
        self.failed_tests = 0
        self.__time = None
        self._test = None
        self._test_duration = 0
        self._active_tests = {}

    def addError(self, test, err):
        self.failed_tests += 1
        self._test = test

    def addFailure(self, test, err):
        self.failed_tests += 1
        self._test = test

    def addSuccess(self, test):
        self._test = test

    def addSkip(self, test, reason=None, details=None):
        self._test = test

    def addUnexpectedSuccess(self, test, details=None):
        self.failed_tests += 1
        self._test = test

    def addExpectedFailure(self, test, err=None, details=None):
        self._test = test

    def reportTest(self, test_id, duration):
        if self.show_times:
            seconds = duration.seconds
            seconds += duration.days * 3600 * 24
            seconds += duration.microseconds / 1000000.0
            self._stream.write(test_id + ' %0.3f\n' % seconds)
        else:
            self._stream.write(test_id + '\n')

    def startTest(self, test):
        self._start_time = self._time()

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        if not test_id:
            return
        if timestamp is not None:
            self.time(timestamp)
        if test_status=='exists':
            if self.show_exists:
                self.reportTest(test_id, 0)
        elif test_status in ('inprogress', None):
            self._active_tests[test_id] = self._time()
        else:
            self._end_test(test_id)

    def _end_test(self, test_id):
        test_start = self._active_tests.pop(test_id, None)
        if not test_start:
            test_duration = 0
        else:
            test_duration = self._time() - test_start
        self.reportTest(test_id, test_duration)

    def stopTest(self, test):
        test_duration = self._time() - self._start_time
        self.reportTest(self._test.id(), test_duration)

    def time(self, time):
        self.__time = time

    def _time(self):
        return self.__time

    def wasSuccessful(self):
        "Tells whether or not this result was a success"
        return self.failed_tests == 0

    def stopTestRun(self):
        for test_id in list(self._active_tests.keys()):
            self._end_test(test_id)


class TestByTestResult(testtools.TestResult):
    """Call something every time a test completes."""

# XXX: In testtools since lp:testtools r249.  Once that's released, just
# import that.

    def __init__(self, on_test):
        """Construct a ``TestByTestResult``.

        :param on_test: A callable that take a test case, a status (one of
            "success", "failure", "error", "skip", or "xfail"), a start time
            (a ``datetime`` with timezone), a stop time, an iterable of tags,
            and a details dict. Is called at the end of each test (i.e. on
            ``stopTest``) with the accumulated values for that test.
        """
        super(TestByTestResult, self).__init__()
        self._on_test = on_test

    def startTest(self, test):
        super(TestByTestResult, self).startTest(test)
        self._start_time = self._now()
        # There's no supported (i.e. tested) behaviour that relies on these
        # being set, but it makes me more comfortable all the same. -- jml
        self._status = None
        self._details = None
        self._stop_time = None

    def stopTest(self, test):
        self._stop_time = self._now()
        super(TestByTestResult, self).stopTest(test)
        self._on_test(
            test=test,
            status=self._status,
            start_time=self._start_time,
            stop_time=self._stop_time,
            # current_tags is new in testtools 0.9.13.
            tags=getattr(self, 'current_tags', None),
            details=self._details)

    def _err_to_details(self, test, err, details):
        if details:
            return details
        return {'traceback': TracebackContent(err, test)}

    def addSuccess(self, test, details=None):
        super(TestByTestResult, self).addSuccess(test)
        self._status = 'success'
        self._details = details

    def addFailure(self, test, err=None, details=None):
        super(TestByTestResult, self).addFailure(test, err, details)
        self._status = 'failure'
        self._details = self._err_to_details(test, err, details)

    def addError(self, test, err=None, details=None):
        super(TestByTestResult, self).addError(test, err, details)
        self._status = 'error'
        self._details = self._err_to_details(test, err, details)

    def addSkip(self, test, reason=None, details=None):
        super(TestByTestResult, self).addSkip(test, reason, details)
        self._status = 'skip'
        if details is None:
            details = {'reason': text_content(reason)}
        elif reason:
            # XXX: What if details already has 'reason' key?
            details['reason'] = text_content(reason)
        self._details = details

    def addExpectedFailure(self, test, err=None, details=None):
        super(TestByTestResult, self).addExpectedFailure(test, err, details)
        self._status = 'xfail'
        self._details = self._err_to_details(test, err, details)

    def addUnexpectedSuccess(self, test, details=None):
        super(TestByTestResult, self).addUnexpectedSuccess(test, details)
        self._status = 'success'
        self._details = details


class CsvResult(TestByTestResult):

    def __init__(self, stream):
        super(CsvResult, self).__init__(self._on_test)
        self._write_row = csv.writer(stream).writerow

    def _on_test(self, test, status, start_time, stop_time, tags, details):
        self._write_row([test.id(), status, start_time, stop_time])

    def startTestRun(self):
        super(CsvResult, self).startTestRun()
        self._write_row(['test', 'status', 'start_time', 'stop_time'])


class CatFiles(StreamResult):
    """Cat file attachments received to a stream."""

    def __init__(self, byte_stream):
        self.stream = subunit.make_stream_binary(byte_stream)

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        if file_name is not None:
            self.stream.write(file_bytes)
            self.stream.flush()
