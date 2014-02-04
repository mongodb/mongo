# Copyright (c) 2009-2010 testtools developers. See LICENSE for details.

"""Doubles of test result objects, useful for testing unittest code."""

__all__ = [
    'Python26TestResult',
    'Python27TestResult',
    'ExtendedTestResult',
    'StreamResult',
    ]


from testtools.tags import TagContext


class LoggingBase(object):
    """Basic support for logging of results."""

    def __init__(self):
        self._events = []
        self.shouldStop = False
        self._was_successful = True
        self.testsRun = 0


class Python26TestResult(LoggingBase):
    """A precisely python 2.6 like test result, that logs."""

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

    def __init__(self):
        super(Python27TestResult, self).__init__()
        self.failfast = False

    def addError(self, test, err):
        super(Python27TestResult, self).addError(test, err)
        if self.failfast:
            self.stop()

    def addFailure(self, test, err):
        super(Python27TestResult, self).addFailure(test, err)
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

    def __init__(self):
        super(ExtendedTestResult, self).__init__()
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
        super(ExtendedTestResult, self).startTestRun()
        self._was_successful = True
        self._tags = TagContext()

    def startTest(self, test):
        super(ExtendedTestResult, self).startTest(test)
        self._tags = TagContext(self._tags)

    def stopTest(self, test):
        self._tags = self._tags.parent
        super(ExtendedTestResult, self).stopTest(test)

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


class StreamResult(object):
    """A StreamResult implementation for testing.

    All events are logged to _events.
    """

    def __init__(self):
        self._events = []

    def startTestRun(self):
        self._events.append(('startTestRun',))

    def stopTestRun(self):
        self._events.append(('stopTestRun',))

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        self._events.append(('status', test_id, test_status, test_tags,
            runnable, file_name, file_bytes, eof, mime_type, route_code,
            timestamp))
