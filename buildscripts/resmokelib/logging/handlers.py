"""
Additional handlers that are used as the base classes of the buildlogger
handler.
"""

from __future__ import absolute_import

import json
import logging
import sys
import threading
import warnings

import requests
import requests.auth

try:
    import requests.packages.urllib3.exceptions as urllib3_exceptions
except ImportError:
    # Versions of the requests package prior to 1.2.0 did not vendor the urllib3 package.
    urllib3_exceptions = None

from .. import utils
from ..utils import timer

_TIMEOUT_SECS = 10


class BufferedHandler(logging.Handler):
    """
    A handler class that buffers logging records in memory. Whenever
    each record is added to the buffer, a check is made to see if the
    buffer should be flushed. If it should, then flush() is expected to
    do what's needed.
    """

    def __init__(self, capacity, interval_secs):
        """
        Initializes the handler with the buffer size and timeout after
        which the buffer is flushed regardless.
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

        self.__emit_lock = threading.Lock()  # Prohibits concurrent access to 'self.__emit_buffer'.
        self.__emit_buffer = []

        self.__flush_lock = threading.Lock()  # Serializes callers of self.flush().
        self.__timer = None  # Defer creation until we actually begin to log messages.

    def _new_timer(self):
        """
        Returns a new timer.AlarmClock instance that will call the
        flush() method after 'interval_secs' seconds.
        """

        return timer.AlarmClock(self.interval_secs, self.flush)

    def process_record(self, record):
        """
        Applies a transformation to the record before it gets added to
        the buffer.

        The default implementation returns 'record' unmodified.
        """

        return record

    def emit(self, record):
        """
        Emits a record.

        Append the record to the buffer after it has been transformed by
        process_record(). If the length of the buffer is greater than or
        equal to its capacity, then flush() is called to process the
        buffer.

        After flushing the buffer, the timer is restarted so that it
        will expire after another 'interval_secs' seconds.
        """

        if self.__timer is None:
            self.__timer = self._new_timer()
            self.__timer.start()

        with self.__emit_lock:
            self.__emit_buffer.append(self.process_record(record))
            if len(self.__emit_buffer) >= self.capacity:
                # Trigger the timer thread to cause it to flush the buffer early.
                self.__timer.trigger()

    def flush(self):
        """
        Ensures all logging output has been flushed.
        """

        self.__flush(close_called=False)

    def __flush(self, close_called):
        """
        Ensures all logging output has been flushed.
        """

        with self.__emit_lock:
            buf = self.__emit_buffer
            self.__emit_buffer = []

        # The buffer 'buf' is flushed without holding 'self.__emit_lock' to avoid causing callers of
        # self.emit() to block behind the completion of a potentially long-running flush operation.
        if buf:
            with self.__flush_lock:
                self._flush_buffer_with_lock(buf, close_called)

    def _flush_buffer_with_lock(self, buf, close_called):
        """
        Ensures all logging output has been flushed.
        """

        raise NotImplementedError("_flush_buffer_with_lock must be implemented by BufferedHandler"
                                  " subclasses")

    def close(self):
        """
        Tidies up any resources used by the handler.

        Stops the timer and flushes the buffer.
        """

        if self.__timer is not None:
            self.__timer.dismiss()

        self.__flush(close_called=True)

        logging.Handler.close(self)


class HTTPHandler(object):
    """
    A class which sends data to a web server using POST requests.
    """

    def __init__(self, url_root, username, password):
        """
        Initializes the handler with the necessary authentication
        credentials.
        """

        self.auth_handler = requests.auth.HTTPBasicAuth(username, password)

        self.url_root = url_root

    def _make_url(self, endpoint):
        return "%s/%s/" % (self.url_root.rstrip("/"), endpoint.strip("/"))

    def post(self, endpoint, data=None, headers=None, timeout_secs=_TIMEOUT_SECS):
        """
        Sends a POST request to the specified endpoint with the supplied
        data.

        Returns the response, either as a string or a JSON object based
        on the content type.
        """

        data = utils.default_if_none(data, [])
        data = json.dumps(data, encoding="utf-8")

        headers = utils.default_if_none(headers, {})
        headers["Content-Type"] = "application/json; charset=utf-8"

        url = self._make_url(endpoint)

        # Versions of Python earlier than 2.7.9 do not support certificate validation. So we
        # disable certificate validation for older Python versions.
        should_validate_certificates = sys.version_info >= (2, 7, 9)
        with warnings.catch_warnings():
            if urllib3_exceptions is not None and not should_validate_certificates:
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

            response = requests.post(url,
                                     data=data,
                                     headers=headers,
                                     timeout=timeout_secs,
                                     auth=self.auth_handler,
                                     verify=should_validate_certificates)

        response.raise_for_status()

        if not response.encoding:
            response.encoding = "utf-8"

        headers = response.headers

        if headers["Content-Type"].startswith("application/json"):
            return response.json()

        return response.text
