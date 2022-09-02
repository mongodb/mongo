"""Define handlers for communicating with a buildlogger server."""

import functools
import json
import os
import threading

import requests

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.logging import handlers

CREATE_BUILD_ENDPOINT = "/build"
APPEND_GLOBAL_LOGS_ENDPOINT = "/build/%(build_id)s"
CREATE_TEST_ENDPOINT = "/build/%(build_id)s/test"
APPEND_TEST_LOGS_ENDPOINT = "/build/%(build_id)s/test/%(test_id)s"

_BUILDLOGGER_CONFIG = os.getenv("BUILDLOGGER_CREDENTIALS", "mci.buildlogger")

_SEND_AFTER_LINES = 15000
_SEND_AFTER_SECS = 10

# Initialized by resmokelib.logging.loggers.configure_loggers()
BUILDLOGGER_FALLBACK = None

_INCOMPLETE_LOG_OUTPUT = threading.Event()


def is_log_output_incomplete():  # noqa: D205,D400
    """
    Indicate whether the log output is incomplete.

    Return true if we failed to write all of the log output to the buildlogger server, and return
    false otherwise.
    """
    return _INCOMPLETE_LOG_OUTPUT.is_set()


def set_log_output_incomplete():
    """Indicate that we failed to write all of the log output to the buildlogger server."""
    _INCOMPLETE_LOG_OUTPUT.set()


def _log_on_error(func):
    """Provide decorator that causes exceptions to be logged by the "buildlogger" Logger instance.

    Return the wrapped function's return value, or None if an error was encountered.
    """

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        """Provide wrapper function."""
        try:
            return func(*args, **kwargs)
        except requests.HTTPError as err:
            BUILDLOGGER_FALLBACK.error("Encountered an HTTP error: %s", err)
        except requests.RequestException as err:
            BUILDLOGGER_FALLBACK.error("Encountered a network error: %s", err)
        except:  # pylint: disable=bare-except
            BUILDLOGGER_FALLBACK.exception("Encountered an error.")
        return None

    return wrapper


class _LogsSplitter(object):
    """Class with static methods used to split list of log lines into smaller batches."""

    @staticmethod
    def split_logs(log_lines, max_size):  # noqa: D406,D407,D411,D413
        """Split the log lines into batches of size less than or equal to max_size.

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
            """Compute the encoded JSON size of a log line as part of an array.

            2 is added to each string size to account for the array representation of the logs,
            as each line is preceded by a '[' or a space and followed by a ',' or a ']'.
            """
            return len(json.dumps(line)) + 2

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
    """Base class of the buildlogger handler for global logs and handler for test logs."""

    def __init__(self, build_config, endpoint, capacity=_SEND_AFTER_LINES,
                 interval_secs=_SEND_AFTER_SECS):
        """Initialize the buildlogger handler with the build id and credentials."""

        handlers.BufferedHandler.__init__(self, capacity, interval_secs)

        username = build_config["username"]
        password = build_config["password"]
        self.http_handler = handlers.HTTPHandler(_config.BUILDLOGGER_URL, username, password)

        self.endpoint = endpoint
        self.retry_buffer = []
        # Set a reasonable max payload size in case we don't get a HTTP 413 from LogKeeper
        # before timing out. This limit is intentionally slightly larger than LogKeeper's
        # limit of 32MB so we can still receive a 413 where appropriate but won't cause
        # side effects.
        self.max_size = 33 * 1024 * 1024

    def process_record(self, record):
        """Return a tuple of the time the log record was created, and the message.

        This is necessary because the buildlogger expects the log messages to be
        formatted in JSON as:

            [ [ <log-time-1>, <log-message-1> ],
              [ <log-time-2>, <log-message-2> ],
              ... ]
        """
        msg = self.format(record)
        return (record.created, msg)

    def post(self, *args, **kwargs):
        """Provide convenience method for subclasses to use when making POST requests."""
        return self.http_handler.post(*args, **kwargs)

    def _append_logs(self, log_lines):  # noqa: D406,D407,D413
        """Send a POST request to the handlers endpoint with the logs that have been captured.

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

    def __append_logs_chunk(self, log_lines_chunk):  # noqa: D406,D407,D413
        """Send log lines chunk, handle 413 Request Entity Too Large errors & retry, if necessary.

        Returns:
            The number of log lines that have been successfully sent.
        """
        try:
            self.post(self.endpoint, data=log_lines_chunk)
            return len(log_lines_chunk)
        except requests.HTTPError as err:
            # Handle the "Request Entity Too Large" error, set the max size and retry.
            if err.response.status_code == requests.codes.request_entity_too_large:
                response_data = err.response.json()
                if isinstance(response_data, dict) and "max_size" in response_data:
                    new_max_size = response_data["max_size"]
                    if self.max_size and new_max_size >= self.max_size:
                        BUILDLOGGER_FALLBACK.exception(
                            "Received an HTTP 413 code, but already had max_size set")
                        return 0
                    BUILDLOGGER_FALLBACK.warning(
                        "Received an HTTP 413 code, updating the request max_size to %s",
                        new_max_size)
                    self.max_size = new_max_size
                    return self._append_logs(log_lines_chunk)
            BUILDLOGGER_FALLBACK.error("Encountered an HTTP error: %s", err)
        except requests.RequestException as err:
            BUILDLOGGER_FALLBACK.error("Encountered a network error: %s", err)
        except:  # pylint: disable=bare-except
            BUILDLOGGER_FALLBACK.exception("Encountered an error.")
        return 0

    def _flush_buffer_with_lock(self, buf, close_called):
        """Ensure all logging output has been flushed to the buildlogger server.

        If _append_logs() returns false, then the log messages are added
        to a separate buffer and retried the next time flush() is
        called.
        """

        self.retry_buffer.extend(buf)

        nb_sent = self._append_logs(self.retry_buffer)
        if nb_sent:
            self.retry_buffer = self.retry_buffer[nb_sent:]
        if close_called and self.retry_buffer:
            # The request to the logkeeper returned an error. We discard the log output rather than
            # writing the messages to the fallback logkeeper to avoid putting additional pressure on
            # the Evergreen database.
            BUILDLOGGER_FALLBACK.warning(
                "Failed to flush all log output (%d messages) to logkeeper.",
                len(self.retry_buffer))

            # We set a flag to indicate that we failed to flush all log output to logkeeper so
            # resmoke.py can exit with a special return code.
            set_log_output_incomplete()

            self.retry_buffer = []


class BuildloggerTestHandler(_BaseBuildloggerHandler):
    """Buildlogger handler for the test logs."""

    def __init__(self, build_config, build_id, test_id, capacity=_SEND_AFTER_LINES,
                 interval_secs=_SEND_AFTER_SECS):
        """Initialize the buildlogger handler with the credentials, build id, and test id."""
        endpoint = APPEND_TEST_LOGS_ENDPOINT % {
            "build_id": build_id,
            "test_id": test_id,
        }
        _BaseBuildloggerHandler.__init__(self, build_config, endpoint, capacity, interval_secs)

    @_log_on_error
    def _finish_test(self, failed=False):
        """Send a POST request to the APPEND_TEST_LOGS_ENDPOINT with the test status."""
        self.post(
            self.endpoint, headers={
                "X-Sendlogs-Test-Done": "true",
                "X-Sendlogs-Test-Failed": "true" if failed else "false",
            })

    def close(self, test_failed_flag=False):  # pylint: disable=arguments-differ
        """Close the buildlogger handler."""

        _BaseBuildloggerHandler.close(self, test_failed_flag=test_failed_flag)

        self._finish_test()


class BuildloggerGlobalHandler(_BaseBuildloggerHandler):
    """Buildlogger handler for the global logs."""

    def __init__(self, build_config, build_id, capacity=_SEND_AFTER_LINES,
                 interval_secs=_SEND_AFTER_SECS):
        """Initialize the buildlogger handler with the credentials and build id."""
        endpoint = APPEND_GLOBAL_LOGS_ENDPOINT % {"build_id": build_id}
        _BaseBuildloggerHandler.__init__(self, build_config, endpoint, capacity, interval_secs)


class BuildloggerServer(object):
    """A remote server to which build logs can be sent.

    It is used to retrieve handlers that can then be added to logger
    instances to send the log to the servers.
    """

    @_log_on_error
    def __init__(self):
        """Initialize BuildloggerServer."""
        tmp_globals = {}
        self.config = {}
        exec(
            compile(open(_BUILDLOGGER_CONFIG, "rb").read(), _BUILDLOGGER_CONFIG, 'exec'),
            tmp_globals, self.config)

        # Rename "slavename" to "username" if present.
        if "slavename" in self.config and "username" not in self.config:
            self.config["username"] = self.config["slavename"]
            del self.config["slavename"]

        # Rename "passwd" to "password" if present.
        if "passwd" in self.config and "password" not in self.config:
            self.config["password"] = self.config["passwd"]
            del self.config["passwd"]

    @_log_on_error
    def new_build_id(self, suffix):
        """Return a new build id for sending global logs to."""
        username = self.config["username"]
        password = self.config["password"]
        builder = "%s_%s" % (self.config["builder"], suffix)
        build_num = int(self.config["build_num"])

        handler = handlers.HTTPHandler(url_root=_config.BUILDLOGGER_URL, username=username,
                                       password=password, should_retry=True)

        response = handler.post(
            CREATE_BUILD_ENDPOINT, data={
                "builder": builder,
                "buildnum": build_num,
                "task_id": _config.EVERGREEN_TASK_ID,
            })

        return response["id"]

    @_log_on_error
    def new_test_id(self, build_id, test_filename, test_command):
        """Return a new test id for sending test logs to."""
        handler = handlers.HTTPHandler(url_root=_config.BUILDLOGGER_URL,
                                       username=self.config["username"],
                                       password=self.config["password"], should_retry=True)

        endpoint = CREATE_TEST_ENDPOINT % {"build_id": build_id}
        response = handler.post(
            endpoint, data={
                "test_filename": test_filename,
                "command": test_command,
                "phase": self.config.get("build_phase", "unknown"),
                "task_id": _config.EVERGREEN_TASK_ID,
            })

        return response["id"]

    def get_global_handler(self, build_id, handler_info):
        """Return the global handler."""
        return BuildloggerGlobalHandler(self.config, build_id, **handler_info)

    def get_test_handler(self, build_id, test_id, handler_info):
        """Return the test handler."""
        return BuildloggerTestHandler(self.config, build_id, test_id, **handler_info)

    @staticmethod
    def get_build_log_url(build_id):
        """Return the build log URL."""
        base_url = _config.BUILDLOGGER_URL.rstrip("/")
        endpoint = APPEND_GLOBAL_LOGS_ENDPOINT % {"build_id": build_id}
        return "%s/%s" % (base_url, endpoint.strip("/"))

    @staticmethod
    def get_test_log_url(build_id, test_id):
        """Return the test log URL."""
        base_url = _config.BUILDLOGGER_URL.rstrip("/")
        endpoint = APPEND_TEST_LOGS_ENDPOINT % {"build_id": build_id, "test_id": test_id}
        return "%s/%s" % (base_url, endpoint.strip("/"))
