# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

"""Test results and related things."""

__metaclass__ = type
__all__ = [
    'ExtendedToOriginalDecorator',
    'ExtendedToStreamDecorator',
    'MultiTestResult',
    'StreamFailFast',
    'StreamResult',
    'StreamSummary',
    'StreamTagger',
    'StreamToDict',
    'StreamToExtendedDecorator',
    'StreamToQueue',
    'Tagger',
    'TestControl',
    'TestResult',
    'TestResultDecorator',
    'ThreadsafeForwardingResult',
    'TimestampingStreamResult',
    ]

import datetime
from operator import methodcaller
import sys
import unittest

from extras import safe_hasattr, try_import, try_imports
parse_mime_type = try_import('mimeparse.parse_mime_type')
Queue = try_imports(['Queue.Queue', 'queue.Queue'])

from testtools.compat import all, str_is_unicode, _u, _b
from testtools.content import (
    Content,
    text_content,
    TracebackContent,
    )
from testtools.content_type import ContentType
from testtools.tags import TagContext
# circular import
# from testtools.testcase import PlaceHolder
PlaceHolder = None

# From http://docs.python.org/library/datetime.html
_ZERO = datetime.timedelta(0)

# A UTC class.

class UTC(datetime.tzinfo):
    """UTC"""

    def utcoffset(self, dt):
        return _ZERO

    def tzname(self, dt):
        return "UTC"

    def dst(self, dt):
        return _ZERO

utc = UTC()


class TestResult(unittest.TestResult):
    """Subclass of unittest.TestResult extending the protocol for flexability.

    This test result supports an experimental protocol for providing additional
    data to in test outcomes. All the outcome methods take an optional dict
    'details'. If supplied any other detail parameters like 'err' or 'reason'
    should not be provided. The details dict is a mapping from names to
    MIME content objects (see testtools.content). This permits attaching
    tracebacks, log files, or even large objects like databases that were
    part of the test fixture. Until this API is accepted into upstream
    Python it is considered experimental: it may be replaced at any point
    by a newer version more in line with upstream Python. Compatibility would
    be aimed for in this case, but may not be possible.

    :ivar skip_reasons: A dict of skip-reasons -> list of tests. See addSkip.
    """

    def __init__(self, failfast=False):
        # startTestRun resets all attributes, and older clients don't know to
        # call startTestRun, so it is called once here.
        # Because subclasses may reasonably not expect this, we call the
        # specific version we want to run.
        self.failfast = failfast
        TestResult.startTestRun(self)

    def addExpectedFailure(self, test, err=None, details=None):
        """Called when a test has failed in an expected manner.

        Like with addSuccess and addError, testStopped should still be called.

        :param test: The test that has been skipped.
        :param err: The exc_info of the error that was raised.
        :return: None
        """
        # This is the python 2.7 implementation
        self.expectedFailures.append(
            (test, self._err_details_to_string(test, err, details)))

    def addError(self, test, err=None, details=None):
        """Called when an error has occurred. 'err' is a tuple of values as
        returned by sys.exc_info().

        :param details: Alternative way to supply details about the outcome.
            see the class docstring for more information.
        """
        self.errors.append((test,
            self._err_details_to_string(test, err, details)))
        if self.failfast:
            self.stop()

    def addFailure(self, test, err=None, details=None):
        """Called when an error has occurred. 'err' is a tuple of values as
        returned by sys.exc_info().

        :param details: Alternative way to supply details about the outcome.
            see the class docstring for more information.
        """
        self.failures.append((test,
            self._err_details_to_string(test, err, details)))
        if self.failfast:
            self.stop()

    def addSkip(self, test, reason=None, details=None):
        """Called when a test has been skipped rather than running.

        Like with addSuccess and addError, testStopped should still be called.

        This must be called by the TestCase. 'addError' and 'addFailure' will
        not call addSkip, since they have no assumptions about the kind of
        errors that a test can raise.

        :param test: The test that has been skipped.
        :param reason: The reason for the test being skipped. For instance,
            u"pyGL is not available".
        :param details: Alternative way to supply details about the outcome.
            see the class docstring for more information.
        :return: None
        """
        if reason is None:
            reason = details.get('reason')
            if reason is None:
                reason = 'No reason given'
            else:
                reason = reason.as_text()
        skip_list = self.skip_reasons.setdefault(reason, [])
        skip_list.append(test)

    def addSuccess(self, test, details=None):
        """Called when a test succeeded."""

    def addUnexpectedSuccess(self, test, details=None):
        """Called when a test was expected to fail, but succeed."""
        self.unexpectedSuccesses.append(test)
        if self.failfast:
            self.stop()

    def wasSuccessful(self):
        """Has this result been successful so far?

        If there have been any errors, failures or unexpected successes,
        return False.  Otherwise, return True.

        Note: This differs from standard unittest in that we consider
        unexpected successes to be equivalent to failures, rather than
        successes.
        """
        return not (self.errors or self.failures or self.unexpectedSuccesses)

    def _err_details_to_string(self, test, err=None, details=None):
        """Convert an error in exc_info form or a contents dict to a string."""
        if err is not None:
            return TracebackContent(err, test).as_text()
        return _details_to_str(details, special='traceback')

    def _exc_info_to_unicode(self, err, test):
        # Deprecated.  Only present because subunit upcalls to it.  See
        # <https://bugs.launchpad.net/testtools/+bug/929063>.
        return TracebackContent(err, test).as_text()

    def _now(self):
        """Return the current 'test time'.

        If the time() method has not been called, this is equivalent to
        datetime.now(), otherwise its the last supplied datestamp given to the
        time() method.
        """
        if self.__now is None:
            return datetime.datetime.now(utc)
        else:
            return self.__now

    def startTestRun(self):
        """Called before a test run starts.

        New in Python 2.7. The testtools version resets the result to a
        pristine condition ready for use in another test run.  Note that this
        is different from Python 2.7's startTestRun, which does nothing.
        """
        # failfast is reset by the super __init__, so stash it.
        failfast = self.failfast
        super(TestResult, self).__init__()
        self.skip_reasons = {}
        self.__now = None
        self._tags = TagContext()
        # -- Start: As per python 2.7 --
        self.expectedFailures = []
        self.unexpectedSuccesses = []
        self.failfast = failfast
        # -- End:   As per python 2.7 --

    def stopTestRun(self):
        """Called after a test run completes

        New in python 2.7
        """

    def startTest(self, test):
        super(TestResult, self).startTest(test)
        self._tags = TagContext(self._tags)

    def stopTest(self, test):
        self._tags = self._tags.parent
        super(TestResult, self).stopTest(test)

    @property
    def current_tags(self):
        """The currently set tags."""
        return self._tags.get_current_tags()

    def tags(self, new_tags, gone_tags):
        """Add and remove tags from the test.

        :param new_tags: A set of tags to be added to the stream.
        :param gone_tags: A set of tags to be removed from the stream.
        """
        self._tags.change_tags(new_tags, gone_tags)

    def time(self, a_datetime):
        """Provide a timestamp to represent the current time.

        This is useful when test activity is time delayed, or happening
        concurrently and getting the system time between API calls will not
        accurately represent the duration of tests (or the whole run).

        Calling time() sets the datetime used by the TestResult object.
        Time is permitted to go backwards when using this call.

        :param a_datetime: A datetime.datetime object with TZ information or
            None to reset the TestResult to gathering time from the system.
        """
        self.__now = a_datetime

    def done(self):
        """Called when the test runner is done.

        deprecated in favour of stopTestRun.
        """


class StreamResult(object):
    """A test result for reporting the activity of a test run.

    Typical use
    -----------

      >>> result = StreamResult()
      >>> result.startTestRun()
      >>> try:
      ...     case.run(result)
      ... finally:
      ...     result.stopTestRun()

    The case object will be either a TestCase or a TestSuite, and
    generally make a sequence of calls like::

      >>> result.status(self.id(), 'inprogress')
      >>> result.status(self.id(), 'success')

    General concepts
    ----------------

    StreamResult is built to process events that are emitted by tests during a
    test run or test enumeration. The test run may be running concurrently, and
    even be spread out across multiple machines.

    All events are timestamped to prevent network buffering or scheduling
    latency causing false timing reports. Timestamps are datetime objects in
    the UTC timezone.

    A route_code is a unicode string that identifies where a particular test
    run. This is optional in the API but very useful when multiplexing multiple
    streams together as it allows identification of interactions between tests
    that were run on the same hardware or in the same test process. Generally
    actual tests never need to bother with this - it is added and processed
    by StreamResult's that do multiplexing / run analysis. route_codes are
    also used to route stdin back to pdb instances.

    The StreamResult base class does no accounting or processing, rather it
    just provides an empty implementation of every method, suitable for use
    as a base class regardless of intent.
    """

    def startTestRun(self):
        """Start a test run.

        This will prepare the test result to process results (which might imply
        connecting to a database or remote machine).
        """

    def stopTestRun(self):
        """Stop a test run.

        This informs the result that no more test updates will be received. At
        this point any test ids that have started and not completed can be
        considered failed-or-hung.
        """

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        """Inform the result about a test status.

        :param test_id: The test whose status is being reported. None to 
            report status about the test run as a whole.
        :param test_status: The status for the test. There are two sorts of
            status - interim and final status events. As many interim events
            can be generated as desired, but only one final event. After a
            final status event any further file or status events from the
            same test_id+route_code may be discarded or associated with a new
            test by the StreamResult. (But no exception will be thrown).

            Interim states:
              * None - no particular status is being reported, or status being
                reported is not associated with a test (e.g. when reporting on
                stdout / stderr chatter).
              * inprogress - the test is currently running. Emitted by tests when
                they start running and at any intermediary point they might
                choose to indicate their continual operation.

            Final states:
              * exists - the test exists. This is used when a test is not being
                executed. Typically this is when querying what tests could be run
                in a test run (which is useful for selecting tests to run).
              * xfail - the test failed but that was expected. This is purely
                informative - the test is not considered to be a failure. 
              * uxsuccess - the test passed but was expected to fail. The test
                will be considered a failure.
              * success - the test has finished without error.
              * fail - the test failed (or errored). The test will be considered
                a failure.
              * skip - the test was selected to run but chose to be skipped. E.g.
                a test dependency was missing. This is purely informative - the
                test is not considered to be a failure.

        :param test_tags: Optional set of tags to apply to the test. Tags
            have no intrinsic meaning - that is up to the test author.
        :param runnable: Allows status reports to mark that they are for
            tests which are not able to be explicitly run. For instance,
            subtests will report themselves as non-runnable.
        :param file_name: The name for the file_bytes. Any unicode string may
            be used. While there is no semantic value attached to the name
            of any attachment, the names 'stdout' and 'stderr' and 'traceback'
            are recommended for use only for output sent to stdout, stderr and
            tracebacks of exceptions. When file_name is supplied, file_bytes
            must be a bytes instance.
        :param file_bytes: A bytes object containing content for the named
            file. This can just be a single chunk of the file - emitting
            another file event with more later. Must be None unleses a
            file_name is supplied.
        :param eof: True if this chunk is the last chunk of the file, any
            additional chunks with the same name should be treated as an error
            and discarded. Ignored unless file_name has been supplied.
        :param mime_type: An optional MIME type for the file. stdout and
            stderr will generally be "text/plain; charset=utf8". If None,
            defaults to application/octet-stream. Ignored unless file_name
            has been supplied.
        """


def domap(*args, **kwargs):
    return list(map(*args, **kwargs))


class CopyStreamResult(StreamResult):
    """Copies all event it receives to multiple results.
    
    This provides an easy facility for combining multiple StreamResults.

    For TestResult the equivalent class was ``MultiTestResult``.
    """

    def __init__(self, targets):
        super(CopyStreamResult, self).__init__()
        self.targets = targets

    def startTestRun(self):
        super(CopyStreamResult, self).startTestRun()
        domap(methodcaller('startTestRun'), self.targets)

    def stopTestRun(self):
        super(CopyStreamResult, self).stopTestRun()
        domap(methodcaller('stopTestRun'), self.targets)

    def status(self, *args, **kwargs):
        super(CopyStreamResult, self).status(*args, **kwargs)
        domap(methodcaller('status', *args, **kwargs), self.targets)


class StreamFailFast(StreamResult):
    """Call the supplied callback if an error is seen in a stream.
    
    An example callback::
    
       def do_something():
           pass
    """

    def __init__(self, on_error):
        self.on_error = on_error

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        if test_status in ('uxsuccess', 'fail'):
            self.on_error()


class StreamResultRouter(StreamResult):
    """A StreamResult that routes events.

    StreamResultRouter forwards received events to another StreamResult object,
    selected by a dynamic forwarding policy. Events where no destination is
    found are forwarded to the fallback StreamResult, or an error is raised.

    Typical use is to construct a router with a fallback and then either
    create up front mapping rules, or create them as-needed from the fallback
    handler::

      >>> router = StreamResultRouter()
      >>> sink = doubles.StreamResult()
      >>> router.add_rule(sink, 'route_code_prefix', route_prefix='0',
      ...     consume_route=True)
      >>> router.status(test_id='foo', route_code='0/1', test_status='uxsuccess')

    StreamResultRouter has no buffering.
    
    When adding routes (and for the fallback) whether to call startTestRun and
    stopTestRun or to not call them is controllable by passing
    'do_start_stop_run'. The default is to call them for the fallback only.
    If a route is added after startTestRun has been called, and
    do_start_stop_run is True then startTestRun is called immediately on the 
    new route sink.

    There is no a-priori defined lookup order for routes: if they are ambiguous
    the behaviour is undefined. Only a single route is chosen for any event.
    """

    _policies = {}

    def __init__(self, fallback=None, do_start_stop_run=True):
        """Construct a StreamResultRouter with optional fallback.

        :param fallback: A StreamResult to forward events to when no route
            exists for them.
        :param do_start_stop_run: If False do not pass startTestRun and
            stopTestRun onto the fallback.
        """
        self.fallback = fallback
        self._route_code_prefixes = {}
        self._test_ids = {}
        # Records sinks that should have do_start_stop_run called on them.
        self._sinks = []
        if do_start_stop_run and fallback:
            self._sinks.append(fallback)
        self._in_run = False

    def startTestRun(self):
        super(StreamResultRouter, self).startTestRun()
        for sink in self._sinks:
            sink.startTestRun()
        self._in_run = True

    def stopTestRun(self):
        super(StreamResultRouter, self).stopTestRun()
        for sink in self._sinks:
            sink.stopTestRun()
        self._in_run = False

    def status(self, **kwargs):
        route_code = kwargs.get('route_code', None)
        test_id = kwargs.get('test_id', None)
        if route_code is not None:
            prefix = route_code.split('/')[0]
        else:
            prefix = route_code
        if prefix in self._route_code_prefixes:
            target, consume_route = self._route_code_prefixes[prefix]
            if route_code is not None and consume_route:
                route_code = route_code[len(prefix) + 1:]
                if not route_code:
                    route_code = None
                kwargs['route_code'] = route_code
        elif test_id in self._test_ids:
            target = self._test_ids[test_id]
        else:
            target = self.fallback
        target.status(**kwargs)

    def add_rule(self, sink, policy, do_start_stop_run=False, **policy_args):
        """Add a rule to route events to sink when they match a given policy.

        :param sink: A StreamResult to receive events.
        :param policy: A routing policy. Valid policies are
            'route_code_prefix' and 'test_id'.
        :param do_start_stop_run: If True then startTestRun and stopTestRun
            events will be passed onto this sink.

        :raises: ValueError if the policy is unknown
        :raises: TypeError if the policy is given arguments it cannot handle.

        ``route_code_prefix`` routes events based on a prefix of the route 
        code in the event. It takes a ``route_prefix`` argument to match on 
        (e.g. '0') and a ``consume_route`` argument, which, if True, removes 
        the prefix from the ``route_code`` when forwarding events.

        ``test_id`` routes events based on the test id.  It takes a single 
        argument, ``test_id``.  Use ``None`` to select non-test events.
        """
        policy_method = StreamResultRouter._policies.get(policy, None)
        if not policy_method:
            raise ValueError("bad policy %r" % (policy,))
        policy_method(self, sink, **policy_args)
        if do_start_stop_run:
            self._sinks.append(sink)
        if self._in_run:
            sink.startTestRun()

    def _map_route_code_prefix(self, sink, route_prefix, consume_route=False):
        if '/' in route_prefix:
            raise TypeError(
                "%r is more than one route step long" % (route_prefix,))
        self._route_code_prefixes[route_prefix] = (sink, consume_route)
    _policies['route_code_prefix'] = _map_route_code_prefix

    def _map_test_id(self, sink, test_id):
        self._test_ids[test_id] = sink
    _policies['test_id'] = _map_test_id


class StreamTagger(CopyStreamResult):
    """Adds or discards tags from StreamResult events."""

    def __init__(self, targets, add=None, discard=None):
        """Create a StreamTagger.

        :param targets: A list of targets to forward events onto.
        :param add: Either None or an iterable of tags to add to each event.
        :param discard: Either None or an iterable of tags to discard from each
            event.
        """
        super(StreamTagger, self).__init__(targets)
        self.add = frozenset(add or ())
        self.discard = frozenset(discard or ())

    def status(self, *args, **kwargs):
        test_tags = kwargs.get('test_tags') or set()
        test_tags.update(self.add)
        test_tags.difference_update(self.discard)
        kwargs['test_tags'] = test_tags or None
        super(StreamTagger, self).status(*args, **kwargs)


class StreamToDict(StreamResult):
    """A specialised StreamResult that emits a callback as tests complete.

    Top level file attachments are simply discarded. Hung tests are detected
    by stopTestRun and notified there and then.

    The callback is passed a dict with the following keys:

      * id: the test id.
      * tags: The tags for the test. A set of unicode strings.
      * details: A dict of file attachments - ``testtools.content.Content``
        objects.
      * status: One of the StreamResult status codes (including inprogress) or
        'unknown' (used if only file events for a test were received...)
      * timestamps: A pair of timestamps - the first one received with this
        test id, and the one in the event that triggered the notification.
        Hung tests have a None for the second end event. Timestamps are not
        compared - their ordering is purely order received in the stream.

    Only the most recent tags observed in the stream are reported.
    """

    def __init__(self, on_test):
        """Create a StreamToDict calling on_test on test completions.

        :param on_test: A callback that accepts one parameter - a dict
            describing a test.
        """
        super(StreamToDict, self).__init__()
        self.on_test = on_test
        if parse_mime_type is None:
            raise ImportError("mimeparse module missing.")

    def startTestRun(self):
        super(StreamToDict, self).startTestRun()
        self._inprogress = {}

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        super(StreamToDict, self).status(test_id, test_status,
            test_tags=test_tags, runnable=runnable, file_name=file_name,
            file_bytes=file_bytes, eof=eof, mime_type=mime_type,
            route_code=route_code, timestamp=timestamp)
        key = self._ensure_key(test_id, route_code, timestamp)
        # update fields
        if not key:
            return
        if test_status is not None:
            self._inprogress[key]['status'] = test_status
        self._inprogress[key]['timestamps'][1] = timestamp
        case = self._inprogress[key]
        if file_name is not None:
            if file_name not in case['details']:
                if mime_type is None:
                    mime_type = 'application/octet-stream'
                primary, sub, parameters = parse_mime_type(mime_type)
                if 'charset' in parameters:
                    if ',' in parameters['charset']:
                        # testtools was emitting a bad encoding, workaround it,
                        # Though this does lose data - probably want to drop
                        # this in a few releases.
                        parameters['charset'] = parameters['charset'][
                            :parameters['charset'].find(',')]
                content_type = ContentType(primary, sub, parameters)
                content_bytes = []
                case['details'][file_name] = Content(
                    content_type, lambda:content_bytes)
            case['details'][file_name].iter_bytes().append(file_bytes)
        if test_tags is not None:
            self._inprogress[key]['tags'] = test_tags
        # notify completed tests.
        if test_status not in (None, 'inprogress'):
            self.on_test(self._inprogress.pop(key))
    
    def stopTestRun(self):
        super(StreamToDict, self).stopTestRun()
        while self._inprogress:
            case = self._inprogress.popitem()[1]
            case['timestamps'][1] = None
            self.on_test(case)

    def _ensure_key(self, test_id, route_code, timestamp):
        if test_id is None:
            return
        key = (test_id, route_code)
        if key not in self._inprogress:
            self._inprogress[key] = {
                'id': test_id,
                'tags': set(),
                'details': {},
                'status': 'unknown',
                'timestamps': [timestamp, None]}
        return key


_status_map = {
    'inprogress': 'addFailure',
    'unknown': 'addFailure',
    'success': 'addSuccess',
    'skip': 'addSkip',
    'fail': 'addFailure',
    'xfail': 'addExpectedFailure',
    'uxsuccess': 'addUnexpectedSuccess',
    }


def test_dict_to_case(test_dict):
    """Convert a test dict into a TestCase object.

    :param test_dict: A test dict as generated by StreamToDict.
    :return: A PlaceHolder test object.
    """
    # Circular import.
    global PlaceHolder
    if PlaceHolder is None:
        from testtools.testcase import PlaceHolder
    outcome = _status_map[test_dict['status']]
    return PlaceHolder(test_dict['id'], outcome=outcome,
        details=test_dict['details'], tags=test_dict['tags'],
        timestamps=test_dict['timestamps'])


class StreamSummary(StreamToDict):
    """A specialised StreamResult that summarises a stream.
    
    The summary uses the same representation as the original
    unittest.TestResult contract, allowing it to be consumed by any test
    runner.
    """

    def __init__(self):
        super(StreamSummary, self).__init__(self._gather_test)
        self._handle_status = {
            'success': self._success,
            'skip': self._skip,
            'exists': self._exists,
            'fail': self._fail,
            'xfail': self._xfail,
            'uxsuccess': self._uxsuccess,
            'unknown': self._incomplete,
            'inprogress': self._incomplete,
            }

    def startTestRun(self):
        super(StreamSummary, self).startTestRun()
        self.failures = []
        self.errors = []
        self.testsRun = 0
        self.skipped = []
        self.expectedFailures = []
        self.unexpectedSuccesses = []

    def wasSuccessful(self):
        """Return False if any failure has occured.

        Note that incomplete tests can only be detected when stopTestRun is
        called, so that should be called before checking wasSuccessful.
        """
        return (not self.failures and not self.errors)

    def _gather_test(self, test_dict):
        if test_dict['status'] == 'exists':
            return
        self.testsRun += 1
        case = test_dict_to_case(test_dict)
        self._handle_status[test_dict['status']](case)

    def _incomplete(self, case):
        self.errors.append((case, "Test did not complete"))

    def _success(self, case):
        pass

    def _skip(self, case):
        if 'reason' not in case._details:
            reason = "Unknown"
        else:
            reason = case._details['reason'].as_text()
        self.skipped.append((case, reason))

    def _exists(self, case):
        pass

    def _fail(self, case):
        message = _details_to_str(case._details, special="traceback")
        self.errors.append((case, message))

    def _xfail(self, case):
        message = _details_to_str(case._details, special="traceback")
        self.expectedFailures.append((case, message))

    def _uxsuccess(self, case):
        case._outcome = 'addUnexpectedSuccess'
        self.unexpectedSuccesses.append(case)


class TestControl(object):
    """Controls a running test run, allowing it to be interrupted.
    
    :ivar shouldStop: If True, tests should not run and should instead
        return immediately. Similarly a TestSuite should check this between
        each test and if set stop dispatching any new tests and return.
    """

    def __init__(self):
        super(TestControl, self).__init__()
        self.shouldStop = False

    def stop(self):
        """Indicate that tests should stop running."""
        self.shouldStop = True


class MultiTestResult(TestResult):
    """A test result that dispatches to many test results."""

    def __init__(self, *results):
        # Setup _results first, as the base class __init__ assigns to failfast.
        self._results = list(map(ExtendedToOriginalDecorator, results))
        super(MultiTestResult, self).__init__()

    def __repr__(self):
        return '<%s (%s)>' % (
            self.__class__.__name__, ', '.join(map(repr, self._results)))

    def _dispatch(self, message, *args, **kwargs):
        return tuple(
            getattr(result, message)(*args, **kwargs)
            for result in self._results)

    def _get_failfast(self):
        return getattr(self._results[0], 'failfast', False)
    def _set_failfast(self, value):
        self._dispatch('__setattr__', 'failfast', value)
    failfast = property(_get_failfast, _set_failfast)

    def _get_shouldStop(self):
        return any(self._dispatch('__getattr__', 'shouldStop'))
    def _set_shouldStop(self, value):
        # Called because we subclass TestResult. Probably should not do that.
        pass
    shouldStop = property(_get_shouldStop, _set_shouldStop)

    def startTest(self, test):
        super(MultiTestResult, self).startTest(test)
        return self._dispatch('startTest', test)

    def stop(self):
        return self._dispatch('stop')

    def stopTest(self, test):
        super(MultiTestResult, self).stopTest(test)
        return self._dispatch('stopTest', test)

    def addError(self, test, error=None, details=None):
        return self._dispatch('addError', test, error, details=details)

    def addExpectedFailure(self, test, err=None, details=None):
        return self._dispatch(
            'addExpectedFailure', test, err, details=details)

    def addFailure(self, test, err=None, details=None):
        return self._dispatch('addFailure', test, err, details=details)

    def addSkip(self, test, reason=None, details=None):
        return self._dispatch('addSkip', test, reason, details=details)

    def addSuccess(self, test, details=None):
        return self._dispatch('addSuccess', test, details=details)

    def addUnexpectedSuccess(self, test, details=None):
        return self._dispatch('addUnexpectedSuccess', test, details=details)

    def startTestRun(self):
        super(MultiTestResult, self).startTestRun()
        return self._dispatch('startTestRun')

    def stopTestRun(self):
        return self._dispatch('stopTestRun')

    def tags(self, new_tags, gone_tags):
        super(MultiTestResult, self).tags(new_tags, gone_tags)
        return self._dispatch('tags', new_tags, gone_tags)

    def time(self, a_datetime):
        return self._dispatch('time', a_datetime)

    def done(self):
        return self._dispatch('done')

    def wasSuccessful(self):
        """Was this result successful?

        Only returns True if every constituent result was successful.
        """
        return all(self._dispatch('wasSuccessful'))


class TextTestResult(TestResult):
    """A TestResult which outputs activity to a text stream."""

    def __init__(self, stream, failfast=False):
        """Construct a TextTestResult writing to stream."""
        super(TextTestResult, self).__init__(failfast=failfast)
        self.stream = stream
        self.sep1 = '=' * 70 + '\n'
        self.sep2 = '-' * 70 + '\n'

    def _delta_to_float(self, a_timedelta):
        return (a_timedelta.days * 86400.0 + a_timedelta.seconds +
            a_timedelta.microseconds / 1000000.0)

    def _show_list(self, label, error_list):
        for test, output in error_list:
            self.stream.write(self.sep1)
            self.stream.write("%s: %s\n" % (label, test.id()))
            self.stream.write(self.sep2)
            self.stream.write(output)

    def startTestRun(self):
        super(TextTestResult, self).startTestRun()
        self.__start = self._now()
        self.stream.write("Tests running...\n")

    def stopTestRun(self):
        if self.testsRun != 1:
            plural = 's'
        else:
            plural = ''
        stop = self._now()
        self._show_list('ERROR', self.errors)
        self._show_list('FAIL', self.failures)
        for test in self.unexpectedSuccesses:
            self.stream.write(
                "%sUNEXPECTED SUCCESS: %s\n%s" % (
                    self.sep1, test.id(), self.sep2))
        self.stream.write("\nRan %d test%s in %.3fs\n" %
            (self.testsRun, plural,
             self._delta_to_float(stop - self.__start)))
        if self.wasSuccessful():
            self.stream.write("OK\n")
        else:
            self.stream.write("FAILED (")
            details = []
            details.append("failures=%d" % (
                sum(map(len, (
                    self.failures, self.errors, self.unexpectedSuccesses)))))
            self.stream.write(", ".join(details))
            self.stream.write(")\n")
        super(TextTestResult, self).stopTestRun()


class ThreadsafeForwardingResult(TestResult):
    """A TestResult which ensures the target does not receive mixed up calls.

    Multiple ``ThreadsafeForwardingResults`` can forward to the same target
    result, and that target result will only ever receive the complete set of
    events for one test at a time.

    This is enforced using a semaphore, which further guarantees that tests
    will be sent atomically even if the ``ThreadsafeForwardingResults`` are in
    different threads.

    ``ThreadsafeForwardingResult`` is typically used by
    ``ConcurrentTestSuite``, which creates one ``ThreadsafeForwardingResult``
    per thread, each of which wraps of the TestResult that
    ``ConcurrentTestSuite.run()`` is called with.

    target.startTestRun() and target.stopTestRun() are called once for each
    ThreadsafeForwardingResult that forwards to the same target. If the target
    takes special action on these events, it should take care to accommodate
    this.

    time() and tags() calls are batched to be adjacent to the test result and
    in the case of tags() are coerced into test-local scope, avoiding the
    opportunity for bugs around global state in the target.
    """

    def __init__(self, target, semaphore):
        """Create a ThreadsafeForwardingResult forwarding to target.

        :param target: A ``TestResult``.
        :param semaphore: A ``threading.Semaphore`` with limit 1.
        """
        TestResult.__init__(self)
        self.result = ExtendedToOriginalDecorator(target)
        self.semaphore = semaphore
        self._test_start = None
        self._global_tags = set(), set()
        self._test_tags = set(), set()

    def __repr__(self):
        return '<%s %r>' % (self.__class__.__name__, self.result)

    def _any_tags(self, tags):
        return bool(tags[0] or tags[1])

    def _add_result_with_semaphore(self, method, test, *args, **kwargs):
        now = self._now()
        self.semaphore.acquire()
        try:
            self.result.time(self._test_start)
            self.result.startTest(test)
            self.result.time(now)
            if self._any_tags(self._global_tags):
                self.result.tags(*self._global_tags)
            if self._any_tags(self._test_tags):
                self.result.tags(*self._test_tags)
            self._test_tags = set(), set()
            try:
                method(test, *args, **kwargs)
            finally:
                self.result.stopTest(test)
        finally:
            self.semaphore.release()
        self._test_start = None

    def addError(self, test, err=None, details=None):
        self._add_result_with_semaphore(self.result.addError,
            test, err, details=details)

    def addExpectedFailure(self, test, err=None, details=None):
        self._add_result_with_semaphore(self.result.addExpectedFailure,
            test, err, details=details)

    def addFailure(self, test, err=None, details=None):
        self._add_result_with_semaphore(self.result.addFailure,
            test, err, details=details)

    def addSkip(self, test, reason=None, details=None):
        self._add_result_with_semaphore(self.result.addSkip,
            test, reason, details=details)

    def addSuccess(self, test, details=None):
        self._add_result_with_semaphore(self.result.addSuccess,
            test, details=details)

    def addUnexpectedSuccess(self, test, details=None):
        self._add_result_with_semaphore(self.result.addUnexpectedSuccess,
            test, details=details)

    def progress(self, offset, whence):
        pass

    def startTestRun(self):
        super(ThreadsafeForwardingResult, self).startTestRun()
        self.semaphore.acquire()
        try:
            self.result.startTestRun()
        finally:
            self.semaphore.release()

    def _get_shouldStop(self):
        self.semaphore.acquire()
        try:
            return self.result.shouldStop
        finally:
            self.semaphore.release()
    def _set_shouldStop(self, value):
        # Another case where we should not subclass TestResult
        pass
    shouldStop = property(_get_shouldStop, _set_shouldStop)

    def stop(self):
        self.semaphore.acquire()
        try:
            self.result.stop()
        finally:
            self.semaphore.release()

    def stopTestRun(self):
        self.semaphore.acquire()
        try:
            self.result.stopTestRun()
        finally:
            self.semaphore.release()

    def done(self):
        self.semaphore.acquire()
        try:
            self.result.done()
        finally:
            self.semaphore.release()

    def startTest(self, test):
        self._test_start = self._now()
        super(ThreadsafeForwardingResult, self).startTest(test)

    def wasSuccessful(self):
        return self.result.wasSuccessful()

    def tags(self, new_tags, gone_tags):
        """See `TestResult`."""
        super(ThreadsafeForwardingResult, self).tags(new_tags, gone_tags)
        if self._test_start is not None:
            self._test_tags = _merge_tags(
                self._test_tags, (new_tags, gone_tags))
        else:
            self._global_tags = _merge_tags(
                self._global_tags, (new_tags, gone_tags))


def _merge_tags(existing, changed):
    new_tags, gone_tags = changed
    result_new = set(existing[0])
    result_gone = set(existing[1])
    result_new.update(new_tags)
    result_new.difference_update(gone_tags)
    result_gone.update(gone_tags)
    result_gone.difference_update(new_tags)
    return result_new, result_gone


class ExtendedToOriginalDecorator(object):
    """Permit new TestResult API code to degrade gracefully with old results.

    This decorates an existing TestResult and converts missing outcomes
    such as addSkip to older outcomes such as addSuccess. It also supports
    the extended details protocol. In all cases the most recent protocol
    is attempted first, and fallbacks only occur when the decorated result
    does not support the newer style of calling.
    """

    def __init__(self, decorated):
        self.decorated = decorated
        self._tags = TagContext()
        # Only used for old TestResults that do not have failfast.
        self._failfast = False

    def __repr__(self):
        return '<%s %r>' % (self.__class__.__name__, self.decorated)

    def __getattr__(self, name):
        return getattr(self.decorated, name)

    def addError(self, test, err=None, details=None):
        try:
            self._check_args(err, details)
            if details is not None:
                try:
                    return self.decorated.addError(test, details=details)
                except TypeError:
                    # have to convert
                    err = self._details_to_exc_info(details)
            return self.decorated.addError(test, err)
        finally:
            if self.failfast:
                self.stop()

    def addExpectedFailure(self, test, err=None, details=None):
        self._check_args(err, details)
        addExpectedFailure = getattr(
            self.decorated, 'addExpectedFailure', None)
        if addExpectedFailure is None:
            return self.addSuccess(test)
        if details is not None:
            try:
                return addExpectedFailure(test, details=details)
            except TypeError:
                # have to convert
                err = self._details_to_exc_info(details)
        return addExpectedFailure(test, err)

    def addFailure(self, test, err=None, details=None):
        try:
            self._check_args(err, details)
            if details is not None:
                try:
                    return self.decorated.addFailure(test, details=details)
                except TypeError:
                    # have to convert
                    err = self._details_to_exc_info(details)
            return self.decorated.addFailure(test, err)
        finally:
            if self.failfast:
                self.stop()

    def addSkip(self, test, reason=None, details=None):
        self._check_args(reason, details)
        addSkip = getattr(self.decorated, 'addSkip', None)
        if addSkip is None:
            return self.decorated.addSuccess(test)
        if details is not None:
            try:
                return addSkip(test, details=details)
            except TypeError:
                # extract the reason if it's available
                try:
                    reason = details['reason'].as_text()
                except KeyError:
                    reason = _details_to_str(details)
        return addSkip(test, reason)

    def addUnexpectedSuccess(self, test, details=None):
        try:
            outcome = getattr(self.decorated, 'addUnexpectedSuccess', None)
            if outcome is None:
                try:
                    test.fail("")
                except test.failureException:
                    return self.addFailure(test, sys.exc_info())
            if details is not None:
                try:
                    return outcome(test, details=details)
                except TypeError:
                    pass
            return outcome(test)
        finally:
            if self.failfast:
                self.stop()

    def addSuccess(self, test, details=None):
        if details is not None:
            try:
                return self.decorated.addSuccess(test, details=details)
            except TypeError:
                pass
        return self.decorated.addSuccess(test)

    def _check_args(self, err, details):
        param_count = 0
        if err is not None:
            param_count += 1
        if details is not None:
            param_count += 1
        if param_count != 1:
            raise ValueError("Must pass only one of err '%s' and details '%s"
                % (err, details))

    def _details_to_exc_info(self, details):
        """Convert a details dict to an exc_info tuple."""
        return (
            _StringException,
            _StringException(_details_to_str(details, special='traceback')),
            None)

    @property
    def current_tags(self):
        return getattr(
            self.decorated, 'current_tags', self._tags.get_current_tags())

    def done(self):
        try:
            return self.decorated.done()
        except AttributeError:
            return

    def _get_failfast(self):
        return getattr(self.decorated, 'failfast', self._failfast)
    def _set_failfast(self, value):
        if safe_hasattr(self.decorated, 'failfast'):
            self.decorated.failfast = value
        else:
            self._failfast = value
    failfast = property(_get_failfast, _set_failfast)

    def progress(self, offset, whence):
        method = getattr(self.decorated, 'progress', None)
        if method is None:
            return
        return method(offset, whence)

    @property
    def shouldStop(self):
        return self.decorated.shouldStop

    def startTest(self, test):
        self._tags = TagContext(self._tags)
        return self.decorated.startTest(test)

    def startTestRun(self):
        self._tags = TagContext()
        try:
            return self.decorated.startTestRun()
        except AttributeError:
            return

    def stop(self):
        return self.decorated.stop()

    def stopTest(self, test):
        self._tags = self._tags.parent
        return self.decorated.stopTest(test)

    def stopTestRun(self):
        try:
            return self.decorated.stopTestRun()
        except AttributeError:
            return

    def tags(self, new_tags, gone_tags):
        method = getattr(self.decorated, 'tags', None)
        if method is not None:
            return method(new_tags, gone_tags)
        else:
            self._tags.change_tags(new_tags, gone_tags)

    def time(self, a_datetime):
        method = getattr(self.decorated, 'time', None)
        if method is None:
            return
        return method(a_datetime)

    def wasSuccessful(self):
        return self.decorated.wasSuccessful()


class ExtendedToStreamDecorator(CopyStreamResult, StreamSummary, TestControl):
    """Permit using old TestResult API code with new StreamResult objects.
    
    This decorates a StreamResult and converts old (Python 2.6 / 2.7 /
    Extended) TestResult API calls into StreamResult calls.

    It also supports regular StreamResult calls, making it safe to wrap around
    any StreamResult.
    """

    def __init__(self, decorated):
        super(ExtendedToStreamDecorator, self).__init__([decorated])
        # Deal with mismatched base class constructors.
        TestControl.__init__(self)
        self._started = False

    def _get_failfast(self):
        return len(self.targets) == 2
    def _set_failfast(self, value):
        if value:
            if len(self.targets) == 2:
                return
            self.targets.append(StreamFailFast(self.stop))
        else:
            del self.targets[1:]
    failfast = property(_get_failfast, _set_failfast)

    def startTest(self, test):
        if not self._started:
            self.startTestRun()
        self.status(test_id=test.id(), test_status='inprogress', timestamp=self._now())
        self._tags = TagContext(self._tags)

    def stopTest(self, test):
        self._tags = self._tags.parent

    def addError(self, test, err=None, details=None):
        self._check_args(err, details)
        self._convert(test, err, details, 'fail')
    addFailure = addError

    def _convert(self, test, err, details, status, reason=None):
        if not self._started:
            self.startTestRun()
        test_id = test.id()
        now = self._now()
        if err is not None:
            if details is None:
                details = {}
            details['traceback'] = TracebackContent(err, test)
        if details is not None:
            for name, content in details.items():
                mime_type = repr(content.content_type)
                for file_bytes in content.iter_bytes():
                    self.status(file_name=name, file_bytes=file_bytes,
                        mime_type=mime_type, test_id=test_id, timestamp=now)
                self.status(file_name=name, file_bytes=_b(""), eof=True,
                    mime_type=mime_type, test_id=test_id, timestamp=now)
        if reason is not None:
            self.status(file_name='reason', file_bytes=reason.encode('utf8'),
                eof=True, mime_type="text/plain; charset=utf8",
                test_id=test_id, timestamp=now)
        self.status(test_id=test_id, test_status=status,
            test_tags=self.current_tags, timestamp=now)

    def addExpectedFailure(self, test, err=None, details=None):
        self._check_args(err, details)
        self._convert(test, err, details, 'xfail')

    def addSkip(self, test, reason=None, details=None):
        self._convert(test, None, details, 'skip', reason)

    def addUnexpectedSuccess(self, test, details=None):
        self._convert(test, None, details, 'uxsuccess')

    def addSuccess(self, test, details=None):
        self._convert(test, None, details, 'success')

    def _check_args(self, err, details):
        param_count = 0
        if err is not None:
            param_count += 1
        if details is not None:
            param_count += 1
        if param_count != 1:
            raise ValueError("Must pass only one of err '%s' and details '%s"
                % (err, details))

    def startTestRun(self):
        super(ExtendedToStreamDecorator, self).startTestRun()
        self._tags = TagContext()
        self.shouldStop = False
        self.__now = None
        self._started = True

    def stopTest(self, test):
        self._tags = self._tags.parent

    @property
    def current_tags(self):
        """The currently set tags."""
        return self._tags.get_current_tags()

    def tags(self, new_tags, gone_tags):
        """Add and remove tags from the test.

        :param new_tags: A set of tags to be added to the stream.
        :param gone_tags: A set of tags to be removed from the stream.
        """
        self._tags.change_tags(new_tags, gone_tags)

    def _now(self):
        """Return the current 'test time'.

        If the time() method has not been called, this is equivalent to
        datetime.now(), otherwise its the last supplied datestamp given to the
        time() method.
        """
        if self.__now is None:
            return datetime.datetime.now(utc)
        else:
            return self.__now

    def time(self, a_datetime):
        self.__now = a_datetime

    def wasSuccessful(self):
        if not self._started:
            self.startTestRun()
        return super(ExtendedToStreamDecorator, self).wasSuccessful()


class StreamToExtendedDecorator(StreamResult):
    """Convert StreamResult API calls into ExtendedTestResult calls.

    This will buffer all calls for all concurrently active tests, and
    then flush each test as they complete.

    Incomplete tests will be flushed as errors when the test run stops.

    Non test file attachments are accumulated into a test called
    'testtools.extradata' flushed at the end of the run.
    """

    def __init__(self, decorated):
        # ExtendedToOriginalDecorator takes care of thunking details back to
        # exceptions/reasons etc.
        self.decorated = ExtendedToOriginalDecorator(decorated)
        # StreamToDict buffers and gives us individual tests.
        self.hook = StreamToDict(self._handle_tests)

    def status(self, test_id=None, test_status=None, *args, **kwargs):
        if test_status == 'exists':
            return
        self.hook.status(
            test_id=test_id, test_status=test_status, *args, **kwargs)

    def startTestRun(self):
        self.decorated.startTestRun()
        self.hook.startTestRun()

    def stopTestRun(self):
        self.hook.stopTestRun()
        self.decorated.stopTestRun()

    def _handle_tests(self, test_dict):
        case = test_dict_to_case(test_dict)
        case.run(self.decorated)


class StreamToQueue(StreamResult):
    """A StreamResult which enqueues events as a dict to a queue.Queue.

    Events have their route code updated to include the route code
    StreamToQueue was constructed with before they are submitted. If the event
    route code is None, it is replaced with the StreamToQueue route code,
    otherwise it is prefixed with the supplied code + a hyphen.

    startTestRun and stopTestRun are forwarded to the queue. Implementors that
    dequeue events back into StreamResult calls should take care not to call
    startTestRun / stopTestRun on other StreamResult objects multiple times
    (e.g. by filtering startTestRun and stopTestRun).

    ``StreamToQueue`` is typically used by
    ``ConcurrentStreamTestSuite``, which creates one ``StreamToQueue``
    per thread, forwards status events to the the StreamResult that
    ``ConcurrentStreamTestSuite.run()`` was called with, and uses the
    stopTestRun event to trigger calling join() on the each thread.

    Unlike ThreadsafeForwardingResult which this supercedes, no buffering takes
    place - any event supplied to a StreamToQueue will be inserted into the
    queue immediately.

    Events are forwarded as a dict with a key ``event`` which is one of
    ``startTestRun``, ``stopTestRun`` or ``status``. When ``event`` is 
    ``status`` the dict also has keys matching the keyword arguments
    of ``StreamResult.status``, otherwise it has one other key ``result`` which
    is the result that invoked ``startTestRun``.
    """

    def __init__(self, queue, routing_code):
        """Create a StreamToQueue forwarding to target.

        :param queue: A ``queue.Queue`` to receive events.
        :param routing_code: The routing code to apply to messages.
        """
        super(StreamToQueue, self).__init__()
        self.queue = queue
        self.routing_code = routing_code

    def startTestRun(self):
        self.queue.put(dict(event='startTestRun', result=self))

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        self.queue.put(dict(event='status', test_id=test_id,
            test_status=test_status, test_tags=test_tags, runnable=runnable,
            file_name=file_name, file_bytes=file_bytes, eof=eof,
            mime_type=mime_type, route_code=self.route_code(route_code),
            timestamp=timestamp))

    def stopTestRun(self):
        self.queue.put(dict(event='stopTestRun', result=self))

    def route_code(self, route_code):
        """Adjust route_code on the way through."""
        if route_code is None:
            return self.routing_code
        return self.routing_code + _u("/") + route_code


class TestResultDecorator(object):
    """General pass-through decorator.

    This provides a base that other TestResults can inherit from to
    gain basic forwarding functionality.
    """

    def __init__(self, decorated):
        """Create a TestResultDecorator forwarding to decorated."""
        self.decorated = decorated

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

    def progress(self, offset, whence):
        return self.decorated.progress(offset, whence)

    def wasSuccessful(self):
        return self.decorated.wasSuccessful()

    @property
    def current_tags(self):
        return self.decorated.current_tags

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


class Tagger(TestResultDecorator):
    """Tag each test individually."""

    def __init__(self, decorated, new_tags, gone_tags):
        """Wrap 'decorated' such that each test is tagged.

        :param new_tags: Tags to be added for each test.
        :param gone_tags: Tags to be removed for each test.
        """
        super(Tagger, self).__init__(decorated)
        self._new_tags = set(new_tags)
        self._gone_tags = set(gone_tags)

    def startTest(self, test):
        super(Tagger, self).startTest(test)
        self.tags(self._new_tags, self._gone_tags)


class TestByTestResult(TestResult):
    """Call something every time a test completes."""

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
        tags = set(self.current_tags)
        super(TestByTestResult, self).stopTest(test)
        self._on_test(
            test=test,
            status=self._status,
            start_time=self._start_time,
            stop_time=self._stop_time,
            tags=tags,
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


class TimestampingStreamResult(CopyStreamResult):
    """A StreamResult decorator that assigns a timestamp when none is present.

    This is convenient for ensuring events are timestamped.
    """

    def __init__(self, target):
        super(TimestampingStreamResult, self).__init__([target])

    def status(self, *args, **kwargs):
        timestamp = kwargs.pop('timestamp', None)
        if timestamp is None:
            timestamp = datetime.datetime.now(utc)
        super(TimestampingStreamResult, self).status(
            *args, timestamp=timestamp, **kwargs)


class _StringException(Exception):
    """An exception made from an arbitrary string."""

    if not str_is_unicode:
        def __init__(self, string):
            if type(string) is not unicode:
                raise TypeError("_StringException expects unicode, got %r" %
                    (string,))
            Exception.__init__(self, string)

        def __str__(self):
            return self.args[0].encode("utf-8")

        def __unicode__(self):
            return self.args[0]
    # For 3.0 and above the default __str__ is fine, so we don't define one.

    def __hash__(self):
        return id(self)

    def __eq__(self, other):
        try:
            return self.args == other.args
        except AttributeError:
            return False


def _format_text_attachment(name, text):
    if '\n' in text:
        return "%s: {{{\n%s\n}}}\n" % (name, text)
    return "%s: {{{%s}}}" % (name, text)


def _details_to_str(details, special=None):
    """Convert a details dict to a string.

    :param details: A dictionary mapping short names to ``Content`` objects.
    :param special: If specified, an attachment that should have special
        attention drawn to it. The primary attachment. Normally it's the
        traceback that caused the test to fail.
    :return: A formatted string that can be included in text test results.
    """
    empty_attachments = []
    binary_attachments = []
    text_attachments = []
    special_content = None
    # sorted is for testing, may want to remove that and use a dict
    # subclass with defined order for items instead.
    for key, content in sorted(details.items()):
        if content.content_type.type != 'text':
            binary_attachments.append((key, content.content_type))
            continue
        text = content.as_text().strip()
        if not text:
            empty_attachments.append(key)
            continue
        # We want the 'special' attachment to be at the bottom.
        if key == special:
            special_content = '%s\n' % (text,)
            continue
        text_attachments.append(_format_text_attachment(key, text))
    if text_attachments and not text_attachments[-1].endswith('\n'):
        text_attachments.append('')
    if special_content:
        text_attachments.append(special_content)
    lines = []
    if binary_attachments:
        lines.append('Binary content:\n')
        for name, content_type in binary_attachments:
            lines.append('  %s (%s)\n' % (name, content_type))
    if empty_attachments:
        lines.append('Empty attachments:\n')
        for name in empty_attachments:
            lines.append('  %s\n' % (name,))
    if (binary_attachments or empty_attachments) and text_attachments:
        lines.append('\n')
    lines.append('\n'.join(text_attachments))
    return _u('').join(lines)
