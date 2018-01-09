"""
Defines handlers for communicating with a buildlogger server.
"""

from __future__ import absolute_import

import functools
import httplib
import urllib2
import json

from . import handlers
from . import loggers
from .. import config as _config


CREATE_BUILD_ENDPOINT = "/build"
APPEND_GLOBAL_LOGS_ENDPOINT = "/build/%(build_id)s"
CREATE_TEST_ENDPOINT = "/build/%(build_id)s/test"
APPEND_TEST_LOGS_ENDPOINT = "/build/%(build_id)s/test/%(test_id)s"

_BUILDLOGGER_REALM = "buildlogs"
_BUILDLOGGER_CONFIG = "mci.buildlogger"

_SEND_AFTER_LINES = 2000
_SEND_AFTER_SECS = 10


def _log_on_error(func):
    """
    A decorator that causes any exceptions to be logged by the
    "buildlogger" Logger instance.

    Returns the wrapped function's return value, or None if an error
    was encountered.
    """

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        try:
            return func(*args, **kwargs)
        except urllib2.HTTPError as err:
            sb = []  # String builder.
            sb.append("HTTP Error %s: %s" % (err.code, err.msg))
            sb.append("POST %s" % (err.filename))

            for name in err.hdrs:
                value = err.hdrs[name]
                sb.append("  %s: %s" % (name, value))

            # Try to read the response back from the server.
            if hasattr(err, "read"):
                sb.append(err.read())

            loggers._BUILDLOGGER_FALLBACK.exception("\n".join(sb))
        except:
            loggers._BUILDLOGGER_FALLBACK.exception("Encountered an error.")
        return None

    return wrapper

@_log_on_error
def get_config():
    """
    Returns the buildlogger configuration as evaluated from the
    _BUILDLOGGER_CONFIG file.
    """

    tmp_globals = {}  # Avoid conflicts with variables defined in 'config_file'.
    config = {}
    execfile(_BUILDLOGGER_CONFIG, tmp_globals, config)

    # Rename "slavename" to "username" if present.
    if "slavename" in config and "username" not in config:
        config["username"] = config["slavename"]
        del config["slavename"]
    # Rename "passwd" to "password" if present.
    if "passwd" in config and "password" not in config:
        config["password"] = config["passwd"]
        del config["passwd"]

    return config

@_log_on_error
def new_build_id(config):
    """
    Returns a new build id for sending global logs to.
    """

    if config is None:
        return None

    username = config["username"]
    password = config["password"]
    builder = config["builder"]
    build_num = int(config["build_num"])

    handler = handlers.HTTPHandler(
        realm=_BUILDLOGGER_REALM,
        url_root=_config.BUILDLOGGER_URL,
        username=username,
        password=password)

    response = handler.post(CREATE_BUILD_ENDPOINT, data={
        "builder": builder,
        "buildnum": build_num,
    })

    return response["id"]

@_log_on_error
def new_test_id(build_id, build_config, test_filename, test_command):
    """
    Returns a new test id for sending test logs to.
    """

    if build_id is None or build_config is None:
        return None

    handler = handlers.HTTPHandler(
        realm=_BUILDLOGGER_REALM,
        url_root=_config.BUILDLOGGER_URL,
        username=build_config["username"],
        password=build_config["password"])

    endpoint = CREATE_TEST_ENDPOINT % {"build_id": build_id}
    response = handler.post(endpoint, data={
        "test_filename": test_filename,
        "command": test_command,
        "phase": build_config.get("build_phase", "unknown"),
    })

    return response["id"]


class _LogsSplitter(object):
    """Class with static methods used to split list of log lines into smaller batches."""

    @staticmethod
    def split_logs(log_lines, max_size):
        """
        Splits the log lines into batches of size less than or equal to max_size.

        Args:
            log_lines: A list of log lines.
            max_size: The maximum size in bytes a batch of log lines can have in JSON.
        Returns:
            A list of list of log lines. Each item is a list is a list of log lines
            satisfying the size requirement.
        """
        if not max_size:
            return [log_lines]

        def line_size(line):
            """
            Computes the encoded JSON size of a log line as part of an array.
            2 is added to each string size to account for the array representation of the logs,
            as each line is preceded by a '[' or a space and followed by a ',' or a ']'.
            """
            return len(json.dumps(line, encoding="utf-8")) + 2

        curr_logs = []
        curr_logs_size = 0
        split_logs = []
        for line in log_lines:
            size = line_size(line)
            if curr_logs_size + size > max_size:
                split_logs.append(curr_logs)
                curr_logs = []
                curr_logs_size = 0
            curr_logs.append(line)
            curr_logs_size += size
        split_logs.append(curr_logs)
        return split_logs


class _BaseBuildloggerHandler(handlers.BufferedHandler):
    """
    Base class of the buildlogger handler for the global logs and the
    handler for the test logs.
    """

    def __init__(self,
                 build_config,
                 endpoint,
                 capacity=_SEND_AFTER_LINES,
                 interval_secs=_SEND_AFTER_SECS):
        """
        Initializes the buildlogger handler with the build id and
        credentials.
        """

        handlers.BufferedHandler.__init__(self, capacity, interval_secs)

        username = build_config["username"]
        password = build_config["password"]

        self.http_handler = handlers.HTTPHandler(_BUILDLOGGER_REALM,
                                                 _config.BUILDLOGGER_URL,
                                                 username,
                                                 password)

        self.endpoint = endpoint
        self.retry_buffer = []
        self.max_size = None

    def process_record(self, record):
        """
        Returns a tuple of the time the log record was created, and the
        message because the buildlogger expects the log messages
        formatted in JSON as:

            [ [ <log-time-1>, <log-message-1> ],
              [ <log-time-2>, <log-message-2> ],
              ... ]
        """
        msg = self.format(record)
        return (record.created, msg)

    def post(self, *args, **kwargs):
        """
        Convenience method for subclasses to use when making POST requests.
        """
        return self.http_handler.post(*args, **kwargs)

    def _append_logs(self, log_lines):
        """
        Sends a POST request to the handlers endpoint with the logs that have been captured.

        Returns:
            The number of log lines that have been successfully sent.
        """
        lines_sent = 0
        for chunk in _LogsSplitter.split_logs(log_lines, self.max_size):
            chunk_lines_sent = self.__append_logs_chunk(chunk)
            lines_sent += chunk_lines_sent
            if chunk_lines_sent < len(chunk):
                # Not all lines have been sent. We stop here.
                break
        return lines_sent

    def __append_logs_chunk(self, log_lines_chunk):
        """
        Sends a log lines chunk, handles 413 Request Entity Too Large errors and retries
        if necessary.

        Returns:
            The number of log lines that have been successfully sent.
        """
        try:
            self.post(self.endpoint, data=log_lines_chunk)
            return len(log_lines_chunk)
        except urllib2.HTTPError as err:
            # Handle the "Request Entity Too Large" error, set the max size and retry.
            if err.code == httplib.REQUEST_ENTITY_TOO_LARGE:
                response_data = json.load(err, "utf-8")
                if isinstance(response_data, dict) and "max_size" in response_data:
                    new_max_size = response_data["max_size"]
                    if self.max_size and new_max_size >= self.max_size:
                        loggers._BUILDLOGGER_FALLBACK.exception(
                            "Received an HTTP 413 code, but already had max_size set")
                        return 0
                    loggers._BUILDLOGGER_FALLBACK.warning(
                        "Received an HTTP 413 code, updating the request max_size to %s",
                        new_max_size)
                    self.max_size = new_max_size
                    return self._append_logs(log_lines_chunk)
            loggers._BUILDLOGGER_FALLBACK.exception("Encountered an error.")
            return 0
        except:
            loggers._BUILDLOGGER_FALLBACK.exception("Encountered an error.")
            return 0

    def _flush_buffer_with_lock(self, buf, close_called):
        """
        Ensures all logging output has been flushed to the buildlogger
        server.

        If _append_logs() returns false, then the log messages are added
        to a separate buffer and retried the next time flush() is
        called.
        """

        self.retry_buffer.extend(buf)

        nb_sent = self._append_logs(self.retry_buffer)
        if nb_sent:
            self.retry_buffer = self.retry_buffer[nb_sent:]
        if close_called and self.retry_buffer:
            # Request to the buildlogger server returned an error, so use the fallback logger to
            # avoid losing the log messages entirely.
            for (_, message) in self.retry_buffer:
                # TODO: construct an LogRecord instance equivalent to the one passed to the
                #       process_record() method if we ever decide to log the time when the
                #       LogRecord was created, e.g. using %(asctime)s in
                #       _fallback_buildlogger_handler().
                loggers._BUILDLOGGER_FALLBACK.info(message)
            self.retry_buffer = []


class BuildloggerTestHandler(_BaseBuildloggerHandler):
    """
    Buildlogger handler for the test logs.
    """

    def __init__(self, build_config, build_id, test_id,
                 capacity=_SEND_AFTER_LINES, interval_secs=_SEND_AFTER_SECS):
        """Initializes the buildlogger handler with the credentials, build id, and test id."""
        endpoint = APPEND_TEST_LOGS_ENDPOINT % {
            "build_id": build_id,
            "test_id": test_id,
        }
        _BaseBuildloggerHandler.__init__(self, build_config, endpoint, capacity, interval_secs)

    @_log_on_error
    def _finish_test(self, failed=False):
        """
        Sends a POST request to the APPEND_TEST_LOGS_ENDPOINT with the
        test status.
        """
        self.post(self.endpoint, headers={
            "X-Sendlogs-Test-Done": "true",
            "X-Sendlogs-Test-Failed": "true" if failed else "false",
        })

    def close(self):
        """
        Closes the buildlogger handler.
        """

        _BaseBuildloggerHandler.close(self)

        # TODO: pass the test status (success/failure) to this method
        self._finish_test()


class BuildloggerGlobalHandler(_BaseBuildloggerHandler):
    """
    Buildlogger handler for the global logs.
    """

    def __init__(self, build_config, build_id,
                 capacity=_SEND_AFTER_LINES, interval_secs=_SEND_AFTER_SECS):
        """Initializes the buildlogger handler with the credentials and build id."""
        endpoint = APPEND_GLOBAL_LOGS_ENDPOINT % {"build_id": build_id}
        _BaseBuildloggerHandler.__init__(self, build_config, endpoint, capacity, interval_secs)
