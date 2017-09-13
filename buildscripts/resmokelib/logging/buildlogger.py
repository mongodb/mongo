"""
Defines handlers for communicating with a buildlogger server.
"""

from __future__ import absolute_import

import functools

from . import handlers
from .. import config as _config


CREATE_BUILD_ENDPOINT = "/build"
APPEND_GLOBAL_LOGS_ENDPOINT = "/build/%(build_id)s"
CREATE_TEST_ENDPOINT = "/build/%(build_id)s/test"
APPEND_TEST_LOGS_ENDPOINT = "/build/%(build_id)s/test/%(test_id)s"

_BUILDLOGGER_CONFIG = "mci.buildlogger"

_SEND_AFTER_LINES = 2000
_SEND_AFTER_SECS = 10

# Initialized by resmokelib.logging.loggers.configure_loggers()
BUILDLOGGER_FALLBACK = None


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
        except:
            BUILDLOGGER_FALLBACK.exception("Encountered an error.")
        return None

    return wrapper


class _BaseBuildloggerHandler(handlers.BufferedHandler):
    """
    Base class of the buildlogger handler for the global logs and the
    handler for the test logs.
    """

    def __init__(self,
                 build_id,
                 build_config,
                 capacity=_SEND_AFTER_LINES,
                 interval_secs=_SEND_AFTER_SECS):
        """
        Initializes the buildlogger handler with the build id and
        credentials.
        """

        handlers.BufferedHandler.__init__(self, capacity, interval_secs)

        username = build_config["username"]
        password = build_config["password"]

        self.http_handler = handlers.HTTPHandler(_config.BUILDLOGGER_URL,
                                                 username,
                                                 password)

        self.build_id = build_id
        self.retry_buffer = []

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
        raise NotImplementedError("_append_logs must be implemented by _BaseBuildloggerHandler"
                                  " subclasses")

    def _flush_buffer_with_lock(self, buf, close_called):
        """
        Ensures all logging output has been flushed to the buildlogger
        server.

        If _append_logs() returns false, then the log messages are added
        to a separate buffer and retried the next time flush() is
        called.
        """

        self.retry_buffer.extend(buf)

        if self._append_logs(self.retry_buffer):
            self.retry_buffer = []
        elif close_called:
            # Request to the buildlogger server returned an error, so use the fallback logger to
            # avoid losing the log messages entirely.
            for (_, message) in self.retry_buffer:
                # TODO: construct an LogRecord instance equivalent to the one passed to the
                #       process_record() method if we ever decide to log the time when the
                #       LogRecord was created, e.g. using %(asctime)s in
                #       _fallback_buildlogger_handler().
                BUILDLOGGER_FALLBACK.info(message)
            self.retry_buffer = []


class BuildloggerTestHandler(_BaseBuildloggerHandler):
    """
    Buildlogger handler for the test logs.
    """

    def __init__(self, build_id, build_config, test_id, **kwargs):
        """
        Initializes the buildlogger handler with the build id, test id,
        and credentials.
        """

        _BaseBuildloggerHandler.__init__(self, build_id, build_config, **kwargs)

        self.test_id = test_id

    @_log_on_error
    def _append_logs(self, log_lines):
        """
        Sends a POST request to the APPEND_TEST_LOGS_ENDPOINT with the
        logs that have been captured.
        """
        endpoint = APPEND_TEST_LOGS_ENDPOINT % {
            "build_id": self.build_id,
            "test_id": self.test_id,
        }

        response = self.post(endpoint, data=log_lines)
        return response is not None

    @_log_on_error
    def _finish_test(self, failed=False):
        """
        Sends a POST request to the APPEND_TEST_LOGS_ENDPOINT with the
        test status.
        """
        endpoint = APPEND_TEST_LOGS_ENDPOINT % {
            "build_id": self.build_id,
            "test_id": self.test_id,
        }

        self.post(endpoint, headers={
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

    @_log_on_error
    def _append_logs(self, log_lines):
        """
        Sends a POST request to the APPEND_GLOBAL_LOGS_ENDPOINT with
        the logs that have been captured.
        """
        endpoint = APPEND_GLOBAL_LOGS_ENDPOINT % {"build_id": self.build_id}
        response = self.post(endpoint, data=log_lines)
        return response is not None


class BuildloggerServer(object):
    """A remote server to which build logs can be sent.

    It is used to retrieve handlers that can then be added to logger
    instances to send the log to the servers.
    """

    @_log_on_error
    def __init__(self):
        tmp_globals = {}
        self.config = {}
        execfile(_BUILDLOGGER_CONFIG, tmp_globals, self.config)

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
        """
        Returns a new build id for sending global logs to.
        """
        username = self.config["username"]
        password = self.config["password"]
        builder = "%s_%s" % (self.config["builder"], suffix)
        build_num = int(self.config["build_num"])

        handler = handlers.HTTPHandler(
            url_root=_config.BUILDLOGGER_URL,
            username=username,
            password=password)

        response = handler.post(CREATE_BUILD_ENDPOINT, data={
            "builder": builder,
            "buildnum": build_num,
            "task_id": _config.TASK_ID,
        })

        return response["id"]

    @_log_on_error
    def new_test_id(self, build_id, test_filename, test_command):
        """
        Returns a new test id for sending test logs to.
        """
        handler = handlers.HTTPHandler(
            url_root=_config.BUILDLOGGER_URL,
            username=self.config["username"],
            password=self.config["password"])

        endpoint = CREATE_TEST_ENDPOINT % {"build_id": build_id}
        response = handler.post(endpoint, data={
            "test_filename": test_filename,
            "command": test_command,
            "phase": self.config.get("build_phase", "unknown"),
            "task_id": _config.TASK_ID,
        })

        return response["id"]

    def get_global_handler(self, build_id, handler_info):
        return BuildloggerGlobalHandler(build_id, self.config, **handler_info)

    def get_test_handler(self, build_id, test_id, handler_info):
        return BuildloggerTestHandler(build_id, self.config, test_id, **handler_info)

    @staticmethod
    def get_build_log_url(build_id):
        base_url = _config.BUILDLOGGER_URL.rstrip("/")
        endpoint = APPEND_GLOBAL_LOGS_ENDPOINT % {"build_id": build_id}
        return "%s/%s" % (base_url, endpoint.strip("/"))

    @staticmethod
    def get_test_log_url(build_id, test_id):
        base_url = _config.BUILDLOGGER_URL.rstrip("/")
        endpoint = APPEND_TEST_LOGS_ENDPOINT % {"build_id": build_id, "test_id": test_id}
        return "%s/%s" % (base_url, endpoint.strip("/"))
