#
#  subunit: extensions to Python unittest to get test results from subprocesses.
#  Copyright (C) 2005  Robert Collins <robertc@robertcollins.net>
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

"""Subunit - a streaming test protocol

Overview
++++++++

The ``subunit`` Python package provides a number of ``unittest`` extensions
which can be used to cause tests to output Subunit, to parse Subunit streams
into test activity, perform seamless test isolation within a regular test
case and variously sort, filter and report on test runs.


Key Classes
-----------

The ``subunit.TestProtocolClient`` class is a ``unittest.TestResult``
extension which will translate a test run into a Subunit stream.

The ``subunit.ProtocolTestCase`` class is an adapter between the Subunit wire
protocol and the ``unittest.TestCase`` object protocol. It is used to translate
a stream into a test run, which regular ``unittest.TestResult`` objects can
process and report/inspect.

Subunit has support for non-blocking usage too, for use with asyncore or
Twisted. See the ``TestProtocolServer`` parser class for more details.

Subunit includes extensions to the Python ``TestResult`` protocol. These are
all done in a compatible manner: ``TestResult`` objects that do not implement
the extension methods will not cause errors to be raised, instead the extension
will either lose fidelity (for instance, folding expected failures to success
in Python versions < 2.7 or 3.1), or discard the extended data (for extra
details, tags, timestamping and progress markers).

The test outcome methods ``addSuccess``, ``addError``, ``addExpectedFailure``,
``addFailure``, ``addSkip`` take an optional keyword parameter ``details``
which can be used instead of the usual python unittest parameter.
When used the value of details should be a dict from ``string`` to
``testtools.content.Content`` objects. This is a draft API being worked on with
the Python Testing In Python mail list, with the goal of permitting a common
way to provide additional data beyond a traceback, such as captured data from
disk, logging messages etc. The reference for this API is in testtools (0.9.0
and newer).

The ``tags(new_tags, gone_tags)`` method is called (if present) to add or
remove tags in the test run that is currently executing. If called when no
test is in progress (that is, if called outside of the ``startTest``,
``stopTest`` pair), the the tags apply to all subsequent tests. If called
when a test is in progress, then the tags only apply to that test.

The ``time(a_datetime)`` method is called (if present) when a ``time:``
directive is encountered in a Subunit stream. This is used to tell a TestResult
about the time that events in the stream occurred at, to allow reconstructing
test timing from a stream.

The ``progress(offset, whence)`` method controls progress data for a stream.
The offset parameter is an int, and whence is one of subunit.PROGRESS_CUR,
subunit.PROGRESS_SET, PROGRESS_PUSH, PROGRESS_POP. Push and pop operations
ignore the offset parameter.


Python test support
-------------------

``subunit.run`` is a convenience wrapper to run a Python test suite via
the command line, reporting via Subunit::

  $ python -m subunit.run mylib.tests.test_suite

The ``IsolatedTestSuite`` class is a TestSuite that forks before running its
tests, allowing isolation between the test runner and some tests.

Similarly, ``IsolatedTestCase`` is a base class which can be subclassed to get
tests that will fork() before that individual test is run.

`ExecTestCase`` is a convenience wrapper for running an external
program to get a Subunit stream and then report that back to an arbitrary
result object::

 class AggregateTests(subunit.ExecTestCase):

     def test_script_one(self):
         './bin/script_one'

     def test_script_two(self):
         './bin/script_two'

 # Normally your normal test loading would take of this automatically,
 # It is only spelt out in detail here for clarity.
 suite = unittest.TestSuite([AggregateTests("test_script_one"),
     AggregateTests("test_script_two")])
 # Create any TestResult class you like.
 result = unittest._TextTestResult(sys.stdout)
 # And run your suite as normal, Subunit will exec each external script as
 # needed and report to your result object.
 suite.run(result)

Utility modules
---------------

* subunit.chunked contains HTTP chunked encoding/decoding logic.
* subunit.test_results contains TestResult helper classes.
"""

import os
import re
import subprocess
import sys
import unittest
try:
    from io import UnsupportedOperation as _UnsupportedOperation
except ImportError:
    _UnsupportedOperation = AttributeError

from extras import safe_hasattr
from testtools import content, content_type, ExtendedToOriginalDecorator
from testtools.content import TracebackContent
from testtools.compat import _b, _u, BytesIO, StringIO
try:
    from testtools.testresult.real import _StringException
    RemoteException = _StringException
except ImportError:
    raise ImportError ("testtools.testresult.real does not contain "
        "_StringException, check your version.")
from testtools import testresult, CopyStreamResult

from subunit import chunked, details, iso8601, test_results
from subunit.v2 import ByteStreamToStreamResult, StreamResultToBytes

# same format as sys.version_info: "A tuple containing the five components of
# the version number: major, minor, micro, releaselevel, and serial. All
# values except releaselevel are integers; the release level is 'alpha',
# 'beta', 'candidate', or 'final'. The version_info value corresponding to the
# Python version 2.0 is (2, 0, 0, 'final', 0)."  Additionally we use a
# releaselevel of 'dev' for unreleased under-development code.
#
# If the releaselevel is 'alpha' then the major/minor/micro components are not
# established at this point, and setup.py will use a version of next-$(revno).
# If the releaselevel is 'final', then the tarball will be major.minor.micro.
# Otherwise it is major.minor.micro~$(revno).

__version__ = (0, 0, 16, 'final', 0)

PROGRESS_SET = 0
PROGRESS_CUR = 1
PROGRESS_PUSH = 2
PROGRESS_POP = 3


def test_suite():
    import subunit.tests
    return subunit.tests.test_suite()


def join_dir(base_path, path):
    """
    Returns an absolute path to C{path}, calculated relative to the parent
    of C{base_path}.

    @param base_path: A path to a file or directory.
    @param path: An absolute path, or a path relative to the containing
    directory of C{base_path}.

    @return: An absolute path to C{path}.
    """
    return os.path.join(os.path.dirname(os.path.abspath(base_path)), path)


def tags_to_new_gone(tags):
    """Split a list of tags into a new_set and a gone_set."""
    new_tags = set()
    gone_tags = set()
    for tag in tags:
        if tag[0] == '-':
            gone_tags.add(tag[1:])
        else:
            new_tags.add(tag)
    return new_tags, gone_tags


class DiscardStream(object):
    """A filelike object which discards what is written to it."""

    def fileno(self):
        raise _UnsupportedOperation()

    def write(self, bytes):
        pass

    def read(self, len=0):
        return _b('')


class _ParserState(object):
    """State for the subunit parser."""

    def __init__(self, parser):
        self.parser = parser
        self._test_sym = (_b('test'), _b('testing'))
        self._colon_sym = _b(':')
        self._error_sym = (_b('error'),)
        self._failure_sym = (_b('failure'),)
        self._progress_sym = (_b('progress'),)
        self._skip_sym = _b('skip')
        self._success_sym = (_b('success'), _b('successful'))
        self._tags_sym = (_b('tags'),)
        self._time_sym = (_b('time'),)
        self._xfail_sym = (_b('xfail'),)
        self._uxsuccess_sym = (_b('uxsuccess'),)
        self._start_simple = _u(" [")
        self._start_multipart = _u(" [ multipart")

    def addError(self, offset, line):
        """An 'error:' directive has been read."""
        self.parser.stdOutLineReceived(line)

    def addExpectedFail(self, offset, line):
        """An 'xfail:' directive has been read."""
        self.parser.stdOutLineReceived(line)

    def addFailure(self, offset, line):
        """A 'failure:' directive has been read."""
        self.parser.stdOutLineReceived(line)

    def addSkip(self, offset, line):
        """A 'skip:' directive has been read."""
        self.parser.stdOutLineReceived(line)

    def addSuccess(self, offset, line):
        """A 'success:' directive has been read."""
        self.parser.stdOutLineReceived(line)

    def lineReceived(self, line):
        """a line has been received."""
        parts = line.split(None, 1)
        if len(parts) == 2 and line.startswith(parts[0]):
            cmd, rest = parts
            offset = len(cmd) + 1
            cmd = cmd.rstrip(self._colon_sym)
            if cmd in self._test_sym:
                self.startTest(offset, line)
            elif cmd in self._error_sym:
                self.addError(offset, line)
            elif cmd in self._failure_sym:
                self.addFailure(offset, line)
            elif cmd in self._progress_sym:
                self.parser._handleProgress(offset, line)
            elif cmd in self._skip_sym:
                self.addSkip(offset, line)
            elif cmd in self._success_sym:
                self.addSuccess(offset, line)
            elif cmd in self._tags_sym:
                self.parser._handleTags(offset, line)
                self.parser.subunitLineReceived(line)
            elif cmd in self._time_sym:
                self.parser._handleTime(offset, line)
                self.parser.subunitLineReceived(line)
            elif cmd in self._xfail_sym:
                self.addExpectedFail(offset, line)
            elif cmd in self._uxsuccess_sym:
                self.addUnexpectedSuccess(offset, line)
            else:
                self.parser.stdOutLineReceived(line)
        else:
            self.parser.stdOutLineReceived(line)

    def lostConnection(self):
        """Connection lost."""
        self.parser._lostConnectionInTest(_u('unknown state of '))

    def startTest(self, offset, line):
        """A test start command received."""
        self.parser.stdOutLineReceived(line)


class _InTest(_ParserState):
    """State for the subunit parser after reading a test: directive."""

    def _outcome(self, offset, line, no_details, details_state):
        """An outcome directive has been read.

        :param no_details: Callable to call when no details are presented.
        :param details_state: The state to switch to for details
            processing of this outcome.
        """
        test_name = line[offset:-1].decode('utf8')
        if self.parser.current_test_description == test_name:
            self.parser._state = self.parser._outside_test
            self.parser.current_test_description = None
            no_details()
            self.parser.client.stopTest(self.parser._current_test)
            self.parser._current_test = None
            self.parser.subunitLineReceived(line)
        elif self.parser.current_test_description + self._start_simple == \
            test_name:
            self.parser._state = details_state
            details_state.set_simple()
            self.parser.subunitLineReceived(line)
        elif self.parser.current_test_description + self._start_multipart == \
            test_name:
            self.parser._state = details_state
            details_state.set_multipart()
            self.parser.subunitLineReceived(line)
        else:
            self.parser.stdOutLineReceived(line)

    def _error(self):
        self.parser.client.addError(self.parser._current_test,
            details={})

    def addError(self, offset, line):
        """An 'error:' directive has been read."""
        self._outcome(offset, line, self._error,
            self.parser._reading_error_details)

    def _xfail(self):
        self.parser.client.addExpectedFailure(self.parser._current_test,
            details={})

    def addExpectedFail(self, offset, line):
        """An 'xfail:' directive has been read."""
        self._outcome(offset, line, self._xfail,
            self.parser._reading_xfail_details)

    def _uxsuccess(self):
        self.parser.client.addUnexpectedSuccess(self.parser._current_test)

    def addUnexpectedSuccess(self, offset, line):
        """A 'uxsuccess:' directive has been read."""
        self._outcome(offset, line, self._uxsuccess,
            self.parser._reading_uxsuccess_details)

    def _failure(self):
        self.parser.client.addFailure(self.parser._current_test, details={})

    def addFailure(self, offset, line):
        """A 'failure:' directive has been read."""
        self._outcome(offset, line, self._failure,
            self.parser._reading_failure_details)

    def _skip(self):
        self.parser.client.addSkip(self.parser._current_test, details={})

    def addSkip(self, offset, line):
        """A 'skip:' directive has been read."""
        self._outcome(offset, line, self._skip,
            self.parser._reading_skip_details)

    def _succeed(self):
        self.parser.client.addSuccess(self.parser._current_test, details={})

    def addSuccess(self, offset, line):
        """A 'success:' directive has been read."""
        self._outcome(offset, line, self._succeed,
            self.parser._reading_success_details)

    def lostConnection(self):
        """Connection lost."""
        self.parser._lostConnectionInTest(_u(''))


class _OutSideTest(_ParserState):
    """State for the subunit parser outside of a test context."""

    def lostConnection(self):
        """Connection lost."""

    def startTest(self, offset, line):
        """A test start command received."""
        self.parser._state = self.parser._in_test
        test_name = line[offset:-1].decode('utf8')
        self.parser._current_test = RemotedTestCase(test_name)
        self.parser.current_test_description = test_name
        self.parser.client.startTest(self.parser._current_test)
        self.parser.subunitLineReceived(line)


class _ReadingDetails(_ParserState):
    """Common logic for readin state details."""

    def endDetails(self):
        """The end of a details section has been reached."""
        self.parser._state = self.parser._outside_test
        self.parser.current_test_description = None
        self._report_outcome()
        self.parser.client.stopTest(self.parser._current_test)

    def lineReceived(self, line):
        """a line has been received."""
        self.details_parser.lineReceived(line)
        self.parser.subunitLineReceived(line)

    def lostConnection(self):
        """Connection lost."""
        self.parser._lostConnectionInTest(_u('%s report of ') %
            self._outcome_label())

    def _outcome_label(self):
        """The label to describe this outcome."""
        raise NotImplementedError(self._outcome_label)

    def set_simple(self):
        """Start a simple details parser."""
        self.details_parser = details.SimpleDetailsParser(self)

    def set_multipart(self):
        """Start a multipart details parser."""
        self.details_parser = details.MultipartDetailsParser(self)


class _ReadingFailureDetails(_ReadingDetails):
    """State for the subunit parser when reading failure details."""

    def _report_outcome(self):
        self.parser.client.addFailure(self.parser._current_test,
            details=self.details_parser.get_details())

    def _outcome_label(self):
        return "failure"


class _ReadingErrorDetails(_ReadingDetails):
    """State for the subunit parser when reading error details."""

    def _report_outcome(self):
        self.parser.client.addError(self.parser._current_test,
            details=self.details_parser.get_details())

    def _outcome_label(self):
        return "error"


class _ReadingExpectedFailureDetails(_ReadingDetails):
    """State for the subunit parser when reading xfail details."""

    def _report_outcome(self):
        self.parser.client.addExpectedFailure(self.parser._current_test,
            details=self.details_parser.get_details())

    def _outcome_label(self):
        return "xfail"


class _ReadingUnexpectedSuccessDetails(_ReadingDetails):
    """State for the subunit parser when reading uxsuccess details."""

    def _report_outcome(self):
        self.parser.client.addUnexpectedSuccess(self.parser._current_test,
            details=self.details_parser.get_details())

    def _outcome_label(self):
        return "uxsuccess"


class _ReadingSkipDetails(_ReadingDetails):
    """State for the subunit parser when reading skip details."""

    def _report_outcome(self):
        self.parser.client.addSkip(self.parser._current_test,
            details=self.details_parser.get_details("skip"))

    def _outcome_label(self):
        return "skip"


class _ReadingSuccessDetails(_ReadingDetails):
    """State for the subunit parser when reading success details."""

    def _report_outcome(self):
        self.parser.client.addSuccess(self.parser._current_test,
            details=self.details_parser.get_details("success"))

    def _outcome_label(self):
        return "success"


class TestProtocolServer(object):
    """A parser for subunit.

    :ivar tags: The current tags associated with the protocol stream.
    """

    def __init__(self, client, stream=None, forward_stream=None):
        """Create a TestProtocolServer instance.

        :param client: An object meeting the unittest.TestResult protocol.
        :param stream: The stream that lines received which are not part of the
            subunit protocol should be written to. This allows custom handling
            of mixed protocols. By default, sys.stdout will be used for
            convenience. It should accept bytes to its write() method.
        :param forward_stream: A stream to forward subunit lines to. This
            allows a filter to forward the entire stream while still parsing
            and acting on it. By default forward_stream is set to
            DiscardStream() and no forwarding happens.
        """
        self.client = ExtendedToOriginalDecorator(client)
        if stream is None:
            stream = sys.stdout
            if sys.version_info > (3, 0):
                stream = stream.buffer
        self._stream = stream
        self._forward_stream = forward_stream or DiscardStream()
        # state objects we can switch too
        self._in_test = _InTest(self)
        self._outside_test = _OutSideTest(self)
        self._reading_error_details = _ReadingErrorDetails(self)
        self._reading_failure_details = _ReadingFailureDetails(self)
        self._reading_skip_details = _ReadingSkipDetails(self)
        self._reading_success_details = _ReadingSuccessDetails(self)
        self._reading_xfail_details = _ReadingExpectedFailureDetails(self)
        self._reading_uxsuccess_details = _ReadingUnexpectedSuccessDetails(self)
        # start with outside test.
        self._state = self._outside_test
        # Avoid casts on every call
        self._plusminus = _b('+-')
        self._push_sym = _b('push')
        self._pop_sym = _b('pop')

    def _handleProgress(self, offset, line):
        """Process a progress directive."""
        line = line[offset:].strip()
        if line[0] in self._plusminus:
            whence = PROGRESS_CUR
            delta = int(line)
        elif line == self._push_sym:
            whence = PROGRESS_PUSH
            delta = None
        elif line == self._pop_sym:
            whence = PROGRESS_POP
            delta = None
        else:
            whence = PROGRESS_SET
            delta = int(line)
        self.client.progress(delta, whence)

    def _handleTags(self, offset, line):
        """Process a tags command."""
        tags = line[offset:].decode('utf8').split()
        new_tags, gone_tags = tags_to_new_gone(tags)
        self.client.tags(new_tags, gone_tags)

    def _handleTime(self, offset, line):
        # Accept it, but do not do anything with it yet.
        try:
            event_time = iso8601.parse_date(line[offset:-1])
        except TypeError:
            raise TypeError(_u("Failed to parse %r, got %r")
                % (line, sys.exec_info[1]))
        self.client.time(event_time)

    def lineReceived(self, line):
        """Call the appropriate local method for the received line."""
        self._state.lineReceived(line)

    def _lostConnectionInTest(self, state_string):
        error_string = _u("lost connection during %stest '%s'") % (
            state_string, self.current_test_description)
        self.client.addError(self._current_test, RemoteError(error_string))
        self.client.stopTest(self._current_test)

    def lostConnection(self):
        """The input connection has finished."""
        self._state.lostConnection()

    def readFrom(self, pipe):
        """Blocking convenience API to parse an entire stream.

        :param pipe: A file-like object supporting readlines().
        :return: None.
        """
        for line in pipe.readlines():
            self.lineReceived(line)
        self.lostConnection()

    def _startTest(self, offset, line):
        """Internal call to change state machine. Override startTest()."""
        self._state.startTest(offset, line)

    def subunitLineReceived(self, line):
        self._forward_stream.write(line)

    def stdOutLineReceived(self, line):
        self._stream.write(line)


class TestProtocolClient(testresult.TestResult):
    """A TestResult which generates a subunit stream for a test run.

    # Get a TestSuite or TestCase to run
    suite = make_suite()
    # Create a stream (any object with a 'write' method). This should accept
    # bytes not strings: subunit is a byte orientated protocol.
    stream = file('tests.log', 'wb')
    # Create a subunit result object which will output to the stream
    result = subunit.TestProtocolClient(stream)
    # Optionally, to get timing data for performance analysis, wrap the
    # serialiser with a timing decorator
    result = subunit.test_results.AutoTimingTestResultDecorator(result)
    # Run the test suite reporting to the subunit result object
    suite.run(result)
    # Close the stream.
    stream.close()
    """

    def __init__(self, stream):
        testresult.TestResult.__init__(self)
        stream = make_stream_binary(stream)
        self._stream = stream
        self._progress_fmt = _b("progress: ")
        self._bytes_eol = _b("\n")
        self._progress_plus = _b("+")
        self._progress_push = _b("push")
        self._progress_pop = _b("pop")
        self._empty_bytes = _b("")
        self._start_simple = _b(" [\n")
        self._end_simple = _b("]\n")

    def addError(self, test, error=None, details=None):
        """Report an error in test test.

        Only one of error and details should be provided: conceptually there
        are two separate methods:
            addError(self, test, error)
            addError(self, test, details)

        :param error: Standard unittest positional argument form - an
            exc_info tuple.
        :param details: New Testing-in-python drafted API; a dict from string
            to subunit.Content objects.
        """
        self._addOutcome("error", test, error=error, details=details)
        if self.failfast:
            self.stop()

    def addExpectedFailure(self, test, error=None, details=None):
        """Report an expected failure in test test.

        Only one of error and details should be provided: conceptually there
        are two separate methods:
            addError(self, test, error)
            addError(self, test, details)

        :param error: Standard unittest positional argument form - an
            exc_info tuple.
        :param details: New Testing-in-python drafted API; a dict from string
            to subunit.Content objects.
        """
        self._addOutcome("xfail", test, error=error, details=details)

    def addFailure(self, test, error=None, details=None):
        """Report a failure in test test.

        Only one of error and details should be provided: conceptually there
        are two separate methods:
            addFailure(self, test, error)
            addFailure(self, test, details)

        :param error: Standard unittest positional argument form - an
            exc_info tuple.
        :param details: New Testing-in-python drafted API; a dict from string
            to subunit.Content objects.
        """
        self._addOutcome("failure", test, error=error, details=details)
        if self.failfast:
            self.stop()

    def _addOutcome(self, outcome, test, error=None, details=None,
        error_permitted=True):
        """Report a failure in test test.

        Only one of error and details should be provided: conceptually there
        are two separate methods:
            addOutcome(self, test, error)
            addOutcome(self, test, details)

        :param outcome: A string describing the outcome - used as the
            event name in the subunit stream.
        :param error: Standard unittest positional argument form - an
            exc_info tuple.
        :param details: New Testing-in-python drafted API; a dict from string
            to subunit.Content objects.
        :param error_permitted: If True then one and only one of error or
            details must be supplied. If False then error must not be supplied
            and details is still optional.  """
        self._stream.write(_b("%s: " % outcome) + self._test_id(test))
        if error_permitted:
            if error is None and details is None:
                raise ValueError
        else:
            if error is not None:
                raise ValueError
        if error is not None:
            self._stream.write(self._start_simple)
            tb_content = TracebackContent(error, test)
            for bytes in tb_content.iter_bytes():
                self._stream.write(bytes)
        elif details is not None:
            self._write_details(details)
        else:
            self._stream.write(_b("\n"))
        if details is not None or error is not None:
            self._stream.write(self._end_simple)

    def addSkip(self, test, reason=None, details=None):
        """Report a skipped test."""
        if reason is None:
            self._addOutcome("skip", test, error=None, details=details)
        else:
            self._stream.write(_b("skip: %s [\n" % test.id()))
            self._stream.write(_b("%s\n" % reason))
            self._stream.write(self._end_simple)

    def addSuccess(self, test, details=None):
        """Report a success in a test."""
        self._addOutcome("successful", test, details=details, error_permitted=False)

    def addUnexpectedSuccess(self, test, details=None):
        """Report an unexpected success in test test.

        Details can optionally be provided: conceptually there
        are two separate methods:
            addError(self, test)
            addError(self, test, details)

        :param details: New Testing-in-python drafted API; a dict from string
            to subunit.Content objects.
        """
        self._addOutcome("uxsuccess", test, details=details,
            error_permitted=False)
        if self.failfast:
            self.stop()

    def _test_id(self, test):
        result = test.id()
        if type(result) is not bytes:
            result = result.encode('utf8')
        return result

    def startTest(self, test):
        """Mark a test as starting its test run."""
        super(TestProtocolClient, self).startTest(test)
        self._stream.write(_b("test: ") + self._test_id(test) + _b("\n"))
        self._stream.flush()

    def stopTest(self, test):
        super(TestProtocolClient, self).stopTest(test)
        self._stream.flush()

    def progress(self, offset, whence):
        """Provide indication about the progress/length of the test run.

        :param offset: Information about the number of tests remaining. If
            whence is PROGRESS_CUR, then offset increases/decreases the
            remaining test count. If whence is PROGRESS_SET, then offset
            specifies exactly the remaining test count.
        :param whence: One of PROGRESS_CUR, PROGRESS_SET, PROGRESS_PUSH,
            PROGRESS_POP.
        """
        if whence == PROGRESS_CUR and offset > -1:
            prefix = self._progress_plus
            offset = _b(str(offset))
        elif whence == PROGRESS_PUSH:
            prefix = self._empty_bytes
            offset = self._progress_push
        elif whence == PROGRESS_POP:
            prefix = self._empty_bytes
            offset = self._progress_pop
        else:
            prefix = self._empty_bytes
            offset = _b(str(offset))
        self._stream.write(self._progress_fmt + prefix + offset +
            self._bytes_eol)

    def tags(self, new_tags, gone_tags):
        """Inform the client about tags added/removed from the stream."""
        if not new_tags and not gone_tags:
            return
        tags = set([tag.encode('utf8') for tag in new_tags])
        tags.update([_b("-") + tag.encode('utf8') for tag in gone_tags])
        tag_line = _b("tags: ") + _b(" ").join(tags) + _b("\n")
        self._stream.write(tag_line)

    def time(self, a_datetime):
        """Inform the client of the time.

        ":param datetime: A datetime.datetime object.
        """
        time = a_datetime.astimezone(iso8601.Utc())
        self._stream.write(_b("time: %04d-%02d-%02d %02d:%02d:%02d.%06dZ\n" % (
            time.year, time.month, time.day, time.hour, time.minute,
            time.second, time.microsecond)))

    def _write_details(self, details):
        """Output details to the stream.

        :param details: An extended details dict for a test outcome.
        """
        self._stream.write(_b(" [ multipart\n"))
        for name, content in sorted(details.items()):
            self._stream.write(_b("Content-Type: %s/%s" %
                (content.content_type.type, content.content_type.subtype)))
            parameters = content.content_type.parameters
            if parameters:
                self._stream.write(_b(";"))
                param_strs = []
                for param, value in parameters.items():
                    param_strs.append("%s=%s" % (param, value))
                self._stream.write(_b(",".join(param_strs)))
            self._stream.write(_b("\n%s\n" % name))
            encoder = chunked.Encoder(self._stream)
            list(map(encoder.write, content.iter_bytes()))
            encoder.close()

    def done(self):
        """Obey the testtools result.done() interface."""


def RemoteError(description=_u("")):
    return (_StringException, _StringException(description), None)


class RemotedTestCase(unittest.TestCase):
    """A class to represent test cases run in child processes.

    Instances of this class are used to provide the Python test API a TestCase
    that can be printed to the screen, introspected for metadata and so on.
    However, as they are a simply a memoisation of a test that was actually
    run in the past by a separate process, they cannot perform any interactive
    actions.
    """

    def __eq__ (self, other):
        try:
            return self.__description == other.__description
        except AttributeError:
            return False

    def __init__(self, description):
        """Create a psuedo test case with description description."""
        self.__description = description

    def error(self, label):
        raise NotImplementedError("%s on RemotedTestCases is not permitted." %
            label)

    def setUp(self):
        self.error("setUp")

    def tearDown(self):
        self.error("tearDown")

    def shortDescription(self):
        return self.__description

    def id(self):
        return "%s" % (self.__description,)

    def __str__(self):
        return "%s (%s)" % (self.__description, self._strclass())

    def __repr__(self):
        return "<%s description='%s'>" % \
               (self._strclass(), self.__description)

    def run(self, result=None):
        if result is None: result = self.defaultTestResult()
        result.startTest(self)
        result.addError(self, RemoteError(_u("Cannot run RemotedTestCases.\n")))
        result.stopTest(self)

    def _strclass(self):
        cls = self.__class__
        return "%s.%s" % (cls.__module__, cls.__name__)


class ExecTestCase(unittest.TestCase):
    """A test case which runs external scripts for test fixtures."""

    def __init__(self, methodName='runTest'):
        """Create an instance of the class that will use the named test
           method when executed. Raises a ValueError if the instance does
           not have a method with the specified name.
        """
        unittest.TestCase.__init__(self, methodName)
        testMethod = getattr(self, methodName)
        self.script = join_dir(sys.modules[self.__class__.__module__].__file__,
                               testMethod.__doc__)

    def countTestCases(self):
        return 1

    def run(self, result=None):
        if result is None: result = self.defaultTestResult()
        self._run(result)

    def debug(self):
        """Run the test without collecting errors in a TestResult"""
        self._run(testresult.TestResult())

    def _run(self, result):
        protocol = TestProtocolServer(result)
        process = subprocess.Popen(self.script, shell=True,
            stdout=subprocess.PIPE)
        make_stream_binary(process.stdout)
        output = process.communicate()[0]
        protocol.readFrom(BytesIO(output))


class IsolatedTestCase(unittest.TestCase):
    """A TestCase which executes in a forked process.

    Each test gets its own process, which has a performance overhead but will
    provide excellent isolation from global state (such as django configs,
    zope utilities and so on).
    """

    def run(self, result=None):
        if result is None: result = self.defaultTestResult()
        run_isolated(unittest.TestCase, self, result)


class IsolatedTestSuite(unittest.TestSuite):
    """A TestSuite which runs its tests in a forked process.

    This decorator that will fork() before running the tests and report the
    results from the child process using a Subunit stream.  This is useful for
    handling tests that mutate global state, or are testing C extensions that
    could crash the VM.
    """

    def run(self, result=None):
        if result is None: result = testresult.TestResult()
        run_isolated(unittest.TestSuite, self, result)


def run_isolated(klass, self, result):
    """Run a test suite or case in a subprocess, using the run method on klass.
    """
    c2pread, c2pwrite = os.pipe()
    # fixme - error -> result
    # now fork
    pid = os.fork()
    if pid == 0:
        # Child
        # Close parent's pipe ends
        os.close(c2pread)
        # Dup fds for child
        os.dup2(c2pwrite, 1)
        # Close pipe fds.
        os.close(c2pwrite)

        # at this point, sys.stdin is redirected, now we want
        # to filter it to escape ]'s.
        ### XXX: test and write that bit.
        stream = os.fdopen(1, 'wb')
        result = TestProtocolClient(stream)
        klass.run(self, result)
        stream.flush()
        sys.stderr.flush()
        # exit HARD, exit NOW.
        os._exit(0)
    else:
        # Parent
        # Close child pipe ends
        os.close(c2pwrite)
        # hookup a protocol engine
        protocol = TestProtocolServer(result)
        fileobj = os.fdopen(c2pread, 'rb')
        protocol.readFrom(fileobj)
        os.waitpid(pid, 0)
        # TODO return code evaluation.
    return result


def TAP2SubUnit(tap, output_stream):
    """Filter a TAP pipe into a subunit pipe.

    This should be invoked once per TAP script, as TAP scripts get
    mapped to a single runnable case with multiple components.

    :param tap: A tap pipe/stream/file object - should emit unicode strings.
    :param subunit: A pipe/stream/file object to write subunit results to.
    :return: The exit code to exit with.
    """
    output = StreamResultToBytes(output_stream)
    UTF8_TEXT = 'text/plain; charset=UTF8'
    BEFORE_PLAN = 0
    AFTER_PLAN = 1
    SKIP_STREAM = 2
    state = BEFORE_PLAN
    plan_start = 1
    plan_stop = 0
    # Test data for the next test to emit
    test_name = None
    log = []
    result = None
    def missing_test(plan_start):
        output.status(test_id='test %d' % plan_start,
            test_status='fail', runnable=False, 
            mime_type=UTF8_TEXT, eof=True, file_name="tap meta",
            file_bytes=b"test missing from TAP output")
    def _emit_test():
        "write out a test"
        if test_name is None:
            return
        if log:
            log_bytes = b'\n'.join(log_line.encode('utf8') for log_line in log)
            mime_type = UTF8_TEXT
            file_name = 'tap comment'
            eof = True
        else:
            log_bytes = None
            mime_type = None
            file_name = None
            eof = True
        del log[:]
        output.status(test_id=test_name, test_status=result,
            file_bytes=log_bytes, mime_type=mime_type, eof=eof,
            file_name=file_name, runnable=False)
    for line in tap:
        if state == BEFORE_PLAN:
            match = re.match("(\d+)\.\.(\d+)\s*(?:\#\s+(.*))?\n", line)
            if match:
                state = AFTER_PLAN
                _, plan_stop, comment = match.groups()
                plan_stop = int(plan_stop)
                if plan_start > plan_stop and plan_stop == 0:
                    # skipped file
                    state = SKIP_STREAM
                    output.status(test_id='file skip', test_status='skip',
                        file_bytes=comment.encode('utf8'), eof=True,
                        file_name='tap comment')
                continue
        # not a plan line, or have seen one before
        match = re.match("(ok|not ok)(?:\s+(\d+)?)?(?:\s+([^#]*[^#\s]+)\s*)?(?:\s+#\s+(TODO|SKIP|skip|todo)(?:\s+(.*))?)?\n", line)
        if match:
            # new test, emit current one.
            _emit_test()
            status, number, description, directive, directive_comment = match.groups()
            if status == 'ok':
                result = 'success'
            else:
                result = "fail"
            if description is None:
                description = ''
            else:
                description = ' ' + description
            if directive is not None:
                if directive.upper() == 'TODO':
                    result = 'xfail'
                elif directive.upper() == 'SKIP':
                    result = 'skip'
                if directive_comment is not None:
                    log.append(directive_comment)
            if number is not None:
                number = int(number)
                while plan_start < number:
                    missing_test(plan_start)
                    plan_start += 1
            test_name = "test %d%s" % (plan_start, description)
            plan_start += 1
            continue
        match = re.match("Bail out\!(?:\s*(.*))?\n", line)
        if match:
            reason, = match.groups()
            if reason is None:
                extra = ''
            else:
                extra = ' %s' % reason
            _emit_test()
            test_name = "Bail out!%s" % extra
            result = "fail"
            state = SKIP_STREAM
            continue
        match = re.match("\#.*\n", line)
        if match:
            log.append(line[:-1])
            continue
        # Should look at buffering status and binding this to the prior result.
        output.status(file_bytes=line.encode('utf8'), file_name='stdout',
            mime_type=UTF8_TEXT)
    _emit_test()
    while plan_start <= plan_stop:
        # record missed tests
        missing_test(plan_start)
        plan_start += 1
    return 0


def tag_stream(original, filtered, tags):
    """Alter tags on a stream.

    :param original: The input stream.
    :param filtered: The output stream.
    :param tags: The tags to apply. As in a normal stream - a list of 'TAG' or
        '-TAG' commands.

        A 'TAG' command will add the tag to the output stream,
        and override any existing '-TAG' command in that stream.
        Specifically:
         * A global 'tags: TAG' will be added to the start of the stream.
         * Any tags commands with -TAG will have the -TAG removed.

        A '-TAG' command will remove the TAG command from the stream.
        Specifically:
         * A 'tags: -TAG' command will be added to the start of the stream.
         * Any 'tags: TAG' command will have 'TAG' removed from it.
        Additionally, any redundant tagging commands (adding a tag globally
        present, or removing a tag globally removed) are stripped as a
        by-product of the filtering.
    :return: 0
    """
    new_tags, gone_tags = tags_to_new_gone(tags)
    source = ByteStreamToStreamResult(original, non_subunit_name='stdout')
    class Tagger(CopyStreamResult):
        def status(self, **kwargs):
            tags = kwargs.get('test_tags')
            if not tags:
                tags = set()
            tags.update(new_tags)
            tags.difference_update(gone_tags)
            if tags:
                kwargs['test_tags'] = tags
            else:
                kwargs['test_tags'] = None
            super(Tagger, self).status(**kwargs)
    output = Tagger([StreamResultToBytes(filtered)])
    source.run(output)
    return 0


class ProtocolTestCase(object):
    """Subunit wire protocol to unittest.TestCase adapter.

    ProtocolTestCase honours the core of ``unittest.TestCase`` protocol -
    calling a ProtocolTestCase or invoking the run() method will make a 'test
    run' happen. The 'test run' will simply be a replay of the test activity
    that has been encoded into the stream. The ``unittest.TestCase`` ``debug``
    and ``countTestCases`` methods are not supported because there isn't a
    sensible mapping for those methods.

    # Get a stream (any object with a readline() method), in this case the
    # stream output by the example from ``subunit.TestProtocolClient``.
    stream = file('tests.log', 'rb')
    # Create a parser which will read from the stream and emit
    # activity to a unittest.TestResult when run() is called.
    suite = subunit.ProtocolTestCase(stream)
    # Create a result object to accept the contents of that stream.
    result = unittest._TextTestResult(sys.stdout)
    # 'run' the tests - process the stream and feed its contents to result.
    suite.run(result)
    stream.close()

    :seealso: TestProtocolServer (the subunit wire protocol parser).
    """

    def __init__(self, stream, passthrough=None, forward=None):
        """Create a ProtocolTestCase reading from stream.

        :param stream: A filelike object which a subunit stream can be read
            from.
        :param passthrough: A stream pass non subunit input on to. If not
            supplied, the TestProtocolServer default is used.
        :param forward: A stream to pass subunit input on to. If not supplied
            subunit input is not forwarded.
        """
        stream = make_stream_binary(stream)
        self._stream = stream
        self._passthrough = passthrough
        if forward is not None:
            forward = make_stream_binary(forward)
        self._forward = forward

    def __call__(self, result=None):
        return self.run(result)

    def run(self, result=None):
        if result is None:
            result = self.defaultTestResult()
        protocol = TestProtocolServer(result, self._passthrough, self._forward)
        line = self._stream.readline()
        while line:
            protocol.lineReceived(line)
            line = self._stream.readline()
        protocol.lostConnection()


class TestResultStats(testresult.TestResult):
    """A pyunit TestResult interface implementation for making statistics.

    :ivar total_tests: The total tests seen.
    :ivar passed_tests: The tests that passed.
    :ivar failed_tests: The tests that failed.
    :ivar seen_tags: The tags seen across all tests.
    """

    def __init__(self, stream):
        """Create a TestResultStats which outputs to stream."""
        testresult.TestResult.__init__(self)
        self._stream = stream
        self.failed_tests = 0
        self.skipped_tests = 0
        self.seen_tags = set()

    @property
    def total_tests(self):
        return self.testsRun

    def addError(self, test, err, details=None):
        self.failed_tests += 1

    def addFailure(self, test, err, details=None):
        self.failed_tests += 1

    def addSkip(self, test, reason, details=None):
        self.skipped_tests += 1

    def formatStats(self):
        self._stream.write("Total tests:   %5d\n" % self.total_tests)
        self._stream.write("Passed tests:  %5d\n" % self.passed_tests)
        self._stream.write("Failed tests:  %5d\n" % self.failed_tests)
        self._stream.write("Skipped tests: %5d\n" % self.skipped_tests)
        tags = sorted(self.seen_tags)
        self._stream.write("Seen tags: %s\n" % (", ".join(tags)))

    @property
    def passed_tests(self):
        return self.total_tests - self.failed_tests - self.skipped_tests

    def tags(self, new_tags, gone_tags):
        """Accumulate the seen tags."""
        self.seen_tags.update(new_tags)

    def wasSuccessful(self):
        """Tells whether or not this result was a success"""
        return self.failed_tests == 0


def get_default_formatter():
    """Obtain the default formatter to write to.

    :return: A file-like object.
    """
    formatter = os.getenv("SUBUNIT_FORMATTER")
    if formatter:
        return os.popen(formatter, "w")
    else:
        stream = sys.stdout
        if sys.version_info > (3, 0):
            if safe_hasattr(stream, 'buffer'):
                stream = stream.buffer
        return stream


def read_test_list(path):
    """Read a list of test ids from a file on disk.

    :param path: Path to the file
    :return: Sequence of test ids
    """
    f = open(path, 'rb')
    try:
        return [l.rstrip("\n") for l in f.readlines()]
    finally:
        f.close()


def make_stream_binary(stream):
    """Ensure that a stream will be binary safe. See _make_binary_on_windows.
    
    :return: A binary version of the same stream (some streams cannot be
        'fixed' but can be unwrapped).
    """
    try:
        fileno = stream.fileno()
    except (_UnsupportedOperation, AttributeError):
        pass
    else:
        _make_binary_on_windows(fileno)
    return _unwrap_text(stream)


def _make_binary_on_windows(fileno):
    """Win32 mangles \r\n to \n and that breaks streams. See bug lp:505078."""
    if sys.platform == "win32":
        import msvcrt
        msvcrt.setmode(fileno, os.O_BINARY)


def _unwrap_text(stream):
    """Unwrap stream if it is a text stream to get the original buffer."""
    if sys.version_info > (3, 0):
        unicode_type = str
    else:
        unicode_type = unicode
    try:
        # Read streams
        if type(stream.read(0)) is unicode_type:
            return stream.buffer
    except (_UnsupportedOperation, IOError):
        # Cannot read from the stream: try via writes
        try:
            stream.write(_b(''))
        except TypeError:
            return stream.buffer
    return stream
