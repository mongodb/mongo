# Copyright (c) 2009-2016 testtools developers. See LICENSE for details.

"""Doubles of test result objects, useful for testing unittest code."""

from collections import namedtuple

from testtools.tags import TagContext

__all__ = [
    'Python26TestResult',
    'Python27TestResult',
    'ExtendedTestResult',
    'TwistedTestResult',
    'StreamResult',
    ]


class LoggingBase:
    """Basic support for logging of results."""

    def __init__(self, event_log=None):
        if event_log is None:
            event_log = []
        self._events = event_log


class Python26TestResult(LoggingBase):
    """A precisely python 2.6 like test result, that logs."""

    def __init__(self, event_log=None):
        super().__init__(event_log=event_log)
        self.shouldStop = False
        self._was_successful = True
        self.testsRun = 0

    def addError(self, test, err):
        self._was_successful = False
        self._events.append(('addError', test, err))

    def addFailure(self, test, err):
        self._was_successful = False
        self._events.append(('addFailure', test, err))

    def addSuccess(self, test):
        self._events.append(('addSuccess', test))

    def startTest(self, test):
        self._events.append(('startTest', test))
        self.testsRun += 1

    def stop(self):
        self.shouldStop = True

    def stopTest(self, test):
        self._events.append(('stopTest', test))

    def wasSuccessful(self):
        return self._was_successful


class Python27TestResult(Python26TestResult):
    """A precisely python 2.7 like test result, that logs."""

    def __init__(self, event_log=None):
        super().__init__(event_log)
        self.failfast = False

    def addError(self, test, err):
        super().addError(test, err)
        if self.failfast:
            self.stop()

    def addFailure(self, test, err):
        super().addFailure(test, err)
        if self.failfast:
            self.stop()

    def addExpectedFailure(self, test, err):
        self._events.append(('addExpectedFailure', test, err))

    def addSkip(self, test, reason):
        self._events.append(('addSkip', test, reason))

    def addUnexpectedSuccess(self, test):
        self._events.append(('addUnexpectedSuccess', test))
        if self.failfast:
            self.stop()

    def startTestRun(self):
        self._events.append(('startTestRun',))

    def stopTestRun(self):
        self._events.append(('stopTestRun',))


class ExtendedTestResult(Python27TestResult):
    """A test result like the proposed extended unittest result API."""

    def __init__(self, event_log=None):
        super().__init__(event_log)
        self._tags = TagContext()

    def addError(self, test, err=None, details=None):
        self._was_successful = False
        self._events.append(('addError', test, err or details))

    def addFailure(self, test, err=None, details=None):
        self._was_successful = False
        self._events.append(('addFailure', test, err or details))

    def addExpectedFailure(self, test, err=None, details=None):
        self._events.append(('addExpectedFailure', test, err or details))

    def addSkip(self, test, reason=None, details=None):
        self._events.append(('addSkip', test, reason or details))

    def addSuccess(self, test, details=None):
        if details:
            self._events.append(('addSuccess', test, details))
        else:
            self._events.append(('addSuccess', test))

    def addUnexpectedSuccess(self, test, details=None):
        self._was_successful = False
        if details is not None:
            self._events.append(('addUnexpectedSuccess', test, details))
        else:
            self._events.append(('addUnexpectedSuccess', test))

    def progress(self, offset, whence):
        self._events.append(('progress', offset, whence))

    def startTestRun(self):
        super().startTestRun()
        self._was_successful = True
        self._tags = TagContext()

    def startTest(self, test):
        super().startTest(test)
        self._tags = TagContext(self._tags)

    def stopTest(self, test):
        self._tags = self._tags.parent
        super().stopTest(test)

    @property
    def current_tags(self):
        return self._tags.get_current_tags()

    def tags(self, new_tags, gone_tags):
        self._tags.change_tags(new_tags, gone_tags)
        self._events.append(('tags', new_tags, gone_tags))

    def time(self, time):
        self._events.append(('time', time))

    def wasSuccessful(self):
        return self._was_successful


class TwistedTestResult(LoggingBase):
    """
    Emulate the relevant bits of :py:class:`twisted.trial.itrial.IReporter`.

    Used to ensure that we can use ``trial`` as a test runner.
    """

    def __init__(self, event_log=None):
        super().__init__(event_log=event_log)
        self._was_successful = True
        self.testsRun = 0

    def startTest(self, test):
        self.testsRun += 1
        self._events.append(('startTest', test))

    def stopTest(self, test):
        self._events.append(('stopTest', test))

    def addSuccess(self, test):
        self._events.append(('addSuccess', test))

    def addError(self, test, error):
        self._was_successful = False
        self._events.append(('addError', test, error))

    def addFailure(self, test, error):
        self._was_successful = False
        self._events.append(('addFailure', test, error))

    def addExpectedFailure(self, test, failure, todo=None):
        self._events.append(('addExpectedFailure', test, failure))

    def addUnexpectedSuccess(self, test, todo=None):
        self._events.append(('addUnexpectedSuccess', test))

    def addSkip(self, test, reason):
        self._events.append(('addSkip', test, reason))

    def wasSuccessful(self):
        return self._was_successful

    def done(self):
        pass


class StreamResult(LoggingBase):
    """A StreamResult implementation for testing.

    All events are logged to _events.
    """

    def startTestRun(self):
        self._events.append(('startTestRun',))

    def stopTestRun(self):
        self._events.append(('stopTestRun',))

    def status(self, test_id=None, test_status=None, test_tags=None,
               runnable=True, file_name=None, file_bytes=None, eof=False,
               mime_type=None, route_code=None, timestamp=None):
        self._events.append(
            _StatusEvent(
                'status', test_id, test_status, test_tags, runnable,
                file_name, file_bytes, eof, mime_type, route_code,
                timestamp))


# Convenience for easier access to status fields
_StatusEvent = namedtuple(
    "_Event", [
        "name", "test_id", "test_status", "test_tags", "runnable", "file_name",
        "file_bytes", "eof", "mime_type", "route_code", "timestamp"])
