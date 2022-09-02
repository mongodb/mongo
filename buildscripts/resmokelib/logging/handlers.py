"""Additional handlers that are used as the base classes of the buildlogger handler."""

import json
import logging
import threading
import traceback
import warnings

import requests
import requests.adapters
import requests.auth

try:
    import requests.packages.urllib3.exceptions as urllib3_exceptions
except ImportError:
    # Versions of the requests package prior to 1.2.0 did not vendor the urllib3 package.
    urllib3_exceptions = None

import urllib3.util.retry as urllib3_retry

from buildscripts.resmokelib.logging import flush
from buildscripts.resmokelib import utils

_TIMEOUT_SECS = 55


class BufferedHandler(logging.Handler):
    """A handler class that buffers logging records in memory.

    Whenever each record is added to the buffer, a check is made to see if the buffer
    should be flushed. If it should, then flush() is expected to do what's needed.
    """

    def __init__(self, capacity, interval_secs):
        """Initialize the handler with the buffer size and timeout.

        These values determine when the buffer is flushed regardless.
        """

        logging.Handler.__init__(self)

        if not isinstance(capacity, int):
            raise TypeError("capacity must be an integer")
        elif capacity <= 0:
            raise ValueError("capacity must be a positive integer")

        if not isinstance(interval_secs, (int, float)):
            raise TypeError("interval_secs must be a number")
        elif interval_secs <= 0.0:
            raise ValueError("interval_secs must be a positive number")

        self.capacity = capacity
        self.interval_secs = interval_secs

        # Wait 50 seconds before starting to flush the logs. This allows tests that pass in 30 seconds
        # to not flush.
        self.initial_interval_secs = 50
        self.use_initial_interval = True

        # self.__emit_lock prohibits concurrent access to 'self.__emit_buffer',
        # 'self.__flush_event', and self.__flush_scheduled_by_emit.
        self.__emit_lock = threading.Lock()
        self.__emit_buffer = []
        self.__flush_event = None  # A handle to the event that calls self.flush().
        self.__flush_scheduled_by_emit = False
        self.__close_called = False

        self.__flush_lock = threading.Lock()  # Serializes callers of self.flush().

    # We override createLock(), acquire(), and release() to be no-ops since emit(), flush(), and
    # close() serialize accesses to 'self.__emit_buffer' in a more granular way via
    # 'self.__emit_lock'.
    def createLock(self):
        """Create lock."""
        pass

    def acquire(self):
        """Acquire."""
        pass

    def release(self):
        """Release."""
        pass

    def process_record(self, record):
        """Apply a transformation to the record before it gets added to the buffer.

        The default implementation returns 'record' unmodified.
        """

        return record

    def emit(self, record):
        """Emit a record.

        Append the record to the buffer after it has been transformed by
        process_record(). If the length of the buffer is greater than or
        equal to its capacity, then the flush() event is rescheduled to
        immediately process the buffer.
        """

        processed_record = self.process_record(record)

        with self.__emit_lock:
            self.__emit_buffer.append(processed_record)

            if self.__flush_event is None:
                # Now that we've added our first record to the buffer, we schedule a call to flush()
                # to occur 'self.interval_secs' or `self.initial_interval_secs` seconds from now.
                # 'self.__flush_event' should never be None after this point.
                delay = self.interval_secs
                if self.use_initial_interval:
                    delay = self.initial_interval_secs
                    self.use_initial_interval = False
                self.__flush_event = flush.flush_after(self, delay=delay)

            if not self.__flush_scheduled_by_emit and len(self.__emit_buffer) >= self.capacity:
                # Attempt to flush the buffer early if we haven't already done so. We don't bother
                # calling flush.cancel() and flush.flush_after() when 'self.__flush_event' is
                # already scheduled to happen as soon as possible to avoid introducing unnecessary
                # delays in emit().
                if flush.cancel(self.__flush_event):
                    self.__flush_event = flush.flush_after(self, delay=0.0)
                    self.__flush_scheduled_by_emit = True

    def flush(self):
        """Ensure all logging output has been flushed."""

        self.__flush(close_called=False)

        with self.__emit_lock:
            if self.__flush_event is not None and not self.__close_called:
                # We cancel 'self.__flush_event' in case flush() was called by someone other than
                # the flush thread to avoid having multiple flush() events scheduled.
                flush.cancel(self.__flush_event)
                self.__flush_event = flush.flush_after(self, delay=self.interval_secs)
                self.__flush_scheduled_by_emit = False

    def __flush(self, close_called):
        """Ensure all logging output has been flushed."""

        with self.__emit_lock:
            buf = self.__emit_buffer
            self.__emit_buffer = []

        # The buffer 'buf' is flushed without holding 'self.__emit_lock' to avoid causing callers of
        # self.emit() to block behind the completion of a potentially long-running flush operation.
        if buf:
            with self.__flush_lock:
                self._flush_buffer_with_lock(buf, close_called)

    def _flush_buffer_with_lock(self, buf, close_called):
        """Ensure all logging output has been flushed."""

        raise NotImplementedError("_flush_buffer_with_lock must be implemented by BufferedHandler"
                                  " subclasses")

    def close(self, **kwargs):
        """Flush the buffer and tidies up any resources used by this handler."""

        # Import here after the build id has been set.
        from buildscripts.resmokelib.config import EVERGREEN_BUILD_ID

        with self.__emit_lock:
            self.__close_called = True

            if self.__flush_event is not None:
                flush.cancel(self.__flush_event)

            # Don't flush successful tests in patch builds to save on logkeeper space.
            # Keep the last item so flush doesn't hang on an empty buffer.
            if EVERGREEN_BUILD_ID is not None and "_patch_" in EVERGREEN_BUILD_ID and not kwargs.get(
                    "test_failed_flag", False):
                self.use_initial_interval = True
                self.__emit_buffer = []

        self.__flush(close_called=True)

        logging.Handler.close(self)


class HTTPHandler(object):
    """A class which sends data to a web server using POST requests."""

    def __init__(self, url_root, username, password, should_retry=False):
        """Initialize the handler with the necessary authentication credentials."""

        self.auth_handler = requests.auth.HTTPBasicAuth(username, password)

        self.session = requests.Session()

        if should_retry:
            retry_status = [500, 502, 503, 504]  # Retry for these statuses.
            retry = urllib3_retry.Retry(
                backoff_factor=0.1,  # Enable backoff starting at 0.1s.
                allowed_methods=False,  # Support all HTTP verbs.
                status_forcelist=retry_status)

            adapter = requests.adapters.HTTPAdapter(max_retries=retry)
            self.session.mount('http://', adapter)
            self.session.mount('https://', adapter)

        self.url_root = url_root

    def _make_url(self, endpoint):
        return "%s/%s/" % (self.url_root.rstrip("/"), endpoint.strip("/"))

    def post(self, endpoint, data=None, headers=None, timeout_secs=_TIMEOUT_SECS):
        """Send a POST request to the specified endpoint with the supplied data.

        Return the response, either as a string or a JSON object based
        on the content type.
        """

        data = utils.default_if_none(data, [])
        data = json.dumps(data)

        headers = utils.default_if_none(headers, {})
        headers["Content-Type"] = "application/json; charset=utf-8"

        url = self._make_url(endpoint)

        with warnings.catch_warnings():
            if urllib3_exceptions is not None:
                try:
                    warnings.simplefilter("ignore", urllib3_exceptions.InsecurePlatformWarning)
                except AttributeError:
                    # Versions of urllib3 prior to 1.10.3 didn't define InsecurePlatformWarning.
                    # Versions of requests prior to 2.6.0 didn't have a vendored copy of urllib3
                    # that defined InsecurePlatformWarning.
                    pass

                try:
                    warnings.simplefilter("ignore", urllib3_exceptions.InsecureRequestWarning)
                except AttributeError:
                    # Versions of urllib3 prior to 1.9 didn't define InsecureRequestWarning.
                    # Versions of requests prior to 2.4.0 didn't have a vendored copy of urllib3
                    # that defined InsecureRequestWarning.
                    pass

            response = self.session.post(url, data=data, headers=headers, timeout=timeout_secs,
                                         auth=self.auth_handler, verify=True)

        response.raise_for_status()

        if not response.encoding:
            response.encoding = "utf-8"

        headers = response.headers

        if headers["Content-Type"].startswith("application/json"):
            return response.json()

        return response.text
