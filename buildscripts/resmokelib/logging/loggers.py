"""Module to hold the logger instances themselves."""

import logging
import os
import re
import shutil
import subprocess
import sys

import yaml

from buildscripts.resmokelib import config
from buildscripts.resmokelib.core import redirect as redirect_lib
from buildscripts.resmokelib.logging import formatters
from buildscripts.resmokelib.logging.handlers import (
    BufferedFileHandler,
    ExceptionExtractionHandler,
    ExceptionExtractor,
    Truncate,
)

_DEFAULT_FORMAT = "[%(name)s] %(message)s"

# Executor logger logs information from the testing infrastructure.
EXECUTOR_LOGGER_NAME = "executor"

# Fixture logger captures the log output of server fixtures.
FIXTURE_LOGGER_NAME = "fixture"

# Test logger logs output from actual client-side tests.
TESTS_LOGGER_NAME = "tests"

ROOT_EXECUTOR_LOGGER = None
ROOT_FIXTURE_LOGGER = None
ROOT_TESTS_LOGGER = None

# Maps job nums to fixture loggers.
_FIXTURE_LOGGER_REGISTRY: dict = {}

# URL of parsley logs.
RAW_TEST_LOGS_URL = "https://evergreen.mongodb.com/rest/v2/tasks/{task_id}/build/TestLogs/job{job_num}%2F{test_id}.log?execution={execution}&print_time=true"
RAW_JOBS_LOGS_URL = "https://evergreen.mongodb.com/rest/v2/tasks/{task_id}/build/TestLogs/job{job_num}?execution={execution}&print_time=true"
PARSLEY_JOBS_LOGS_URL = "https://parsley.mongodb.com/test/{task_id}/{execution}/job{job_num}/all"


def _setup_redirects():
    redirect_cmds = []
    redirects = []
    if config.MRLOG:
        redirect_cmds.append(config.MRLOG)

    if config.USER_FRIENDLY_OUTPUT:
        if not config.MRLOG and shutil.which("mrlog"):
            redirect_cmds.append("mrlog")

        redirect_cmds.append(["tee", config.USER_FRIENDLY_OUTPUT])
        redirect_cmds.append(
            [
                "grep",
                "-Ea",
                r"Summary of|Running.*\.\.\.|invariant|fassert|BACKTRACE|Invalid access|Workload\(s\) started|Workload\(s\)|WiredTiger error|AddressSanitizer|threads with tids|failed to load|Completed cmd|Completed stepdown",
            ]
        )

    for idx, redirect in enumerate(redirect_cmds):
        # The first redirect reads from stdout. Otherwise read from the previous redirect.
        stdin = sys.stdout if idx == 0 else redirects[idx - 1].get_stdout()
        # The last redirect writes back to real stdout file descriptor.
        stdout = sys.__stdout__ if idx + 1 == len(redirect_cmds) else subprocess.PIPE
        redirects.append(redirect_lib.Pipe(redirect, stdin, stdout))


def configure_loggers():
    """Configure the loggers and setup redirects."""
    _setup_redirects()

    global ROOT_TESTS_LOGGER
    ROOT_TESTS_LOGGER = new_root_logger(TESTS_LOGGER_NAME)
    global ROOT_FIXTURE_LOGGER
    ROOT_FIXTURE_LOGGER = new_root_logger(FIXTURE_LOGGER_NAME)
    global ROOT_EXECUTOR_LOGGER
    ROOT_EXECUTOR_LOGGER = new_root_logger(EXECUTOR_LOGGER_NAME)

    _write_evergreen_log_spec()


def get_evergreen_log_name(job_num, test_id=None):
    """Return the log name, relative to the reserved test log directory, on the Evergreen task host."""
    return f"job{job_num}/" + (f"{test_id}.log" if test_id else "global.log")


def new_root_logger(name):
    """
    Create and configure a new root logger.

    :param name: The name of the new root logger.
    """
    if name not in config.LOGGING_CONFIG:
        raise ValueError("Logging configuration should contain the %s component" % name)

    logger = logging.getLogger(name)
    logger.setLevel(logging.DEBUG)
    logger_info = config.LOGGING_CONFIG[name]
    formatter = _get_formatter(logger_info)

    for handler_info in logger_info.get("handlers", []):
        _add_handler(logger, handler_info, formatter)

    return logger


def new_resmoke_logger():
    """Create a child logger of the executor logger with the name "resmoke"."""
    logger = logging.Logger("resmoke")
    logger.parent = ROOT_EXECUTOR_LOGGER
    return logger


def new_job_logger(test_kind, job_num) -> logging.Logger:
    """Create a new logger for a given job thread."""
    name = "executor:%s:job%d" % (test_kind, job_num)
    logger = logging.Logger(name)
    logger.parent = ROOT_EXECUTOR_LOGGER

    return logger


# Fixture loggers


class FixtureLogger(logging.Logger):
    """Custom fixture logger."""

    def __init__(self, name, full_name, external_sut_hostname=None):
        """Initialize fixture logger."""
        self.full_name = full_name
        self.external_sut_hostname = external_sut_hostname
        super().__init__(name)


def new_fixture_logger(fixture_class, job_num):
    """Create a logger for a particular fixture class."""
    full_name = "%s:job%d" % (fixture_class, job_num)
    external_sut_hostname = full_name.replace(":", "_").lower()
    logger = FixtureLogger(_shorten(full_name), full_name, external_sut_hostname)
    logger.parent = ROOT_FIXTURE_LOGGER
    _add_evergreen_handler(logger, job_num)

    _FIXTURE_LOGGER_REGISTRY[job_num] = logger
    return logger


def new_fixture_node_logger(fixture_class, job_num, node_name):
    """Create a logger for a particular element in a multi-process fixture."""
    full_name = "%s:job%d:%s" % (fixture_class, job_num, node_name)
    external_sut_hostname = node_name.replace(":", "_")
    logger = FixtureLogger(_shorten(full_name), full_name, external_sut_hostname)
    logger.parent = _FIXTURE_LOGGER_REGISTRY[job_num]
    return logger


# Test loggers


def new_testqueue_logger(test_kind):
    """Create a new test queue logger that associates the test kind to tests."""
    logger = logging.Logger(name=test_kind)
    logger.parent = ROOT_TESTS_LOGGER
    return logger


def configure_exception_capture(test_logger):
    """Configure the test logger to extract exceptions and return the exception extractors."""

    # Capturing exceptions only makes sense for logs in plain format.
    if config.LOG_FORMAT != "plain":
        return []

    js_exception = ExceptionExtractor(
        start_regex=r"^uncaught exception:",
        end_regex=r"^exiting with code",
        truncate=Truncate.LAST,
    )
    py_exception = ExceptionExtractor(
        start_regex=r"^Traceback",
        end_regex=r"^\S*:",
        truncate=Truncate.FIRST,
    )

    # py_exception extracts Python exception messages from the stdout of a Python subprocess.
    # Both it and js_exception are registered as logging.Handlers to avoid specializing which
    # programming language exceptions to expect from certain kinds of tests. Python exceptions
    # from resmoke hooks and fixtures are handled separately through TestReport.addError().
    test_logger.addHandler(ExceptionExtractionHandler(js_exception))
    test_logger.addHandler(ExceptionExtractionHandler(py_exception))

    return [js_exception, py_exception]


def new_test_logger(test_shortname, test_basename, parent, job_num, test_id, job_logger):
    """Create a new test logger that will be a child of the given parent."""
    name = "%s:%s" % (parent.name, test_shortname)
    logger = logging.Logger(name)
    logger.parent = parent
    _add_evergreen_handler(logger, job_num, test_id, test_basename)

    return logger


def new_test_thread_logger(parent, test_kind, thread_id, tenant_id=None):
    """Create a new test thread logger that will be the child of the given parent."""
    name = (
        "%s:%s:%s" % (test_kind, thread_id, tenant_id)
        if tenant_id
        else "%s:%s" % (test_kind, thread_id)
    )
    logger = logging.Logger(name)
    logger.parent = parent
    return logger


def new_hook_logger(hook_class, job_num):
    """Create a new hook logger from a given fixture logger."""
    name = "hook:{}:job{:d}".format(hook_class, job_num)
    logger = logging.Logger(name)
    logger.parent = _FIXTURE_LOGGER_REGISTRY[job_num]
    return logger


# Util methods


def _add_handler(logger, handler_info, formatter):
    """Add handlers to a logger based on configuration."""
    handler_class = handler_info["class"]
    if handler_class == "logging.FileHandler":
        handler = logging.FileHandler(
            filename=handler_info["filename"], mode=handler_info.get("mode", "w")
        )
    elif handler_class == "logging.NullHandler":
        handler = logging.NullHandler()
    elif handler_class == "logging.StreamHandler":
        handler = logging.StreamHandler(sys.stdout)
    elif handler_class == "evergreen":
        return  # Evergreen handlers are applied when creating specific child loggers
    else:
        raise ValueError("Unknown handler class '%s'" % handler_class)
    handler.setFormatter(formatter)
    logger.addHandler(handler)


def _get_formatter(logger_info):
    """Return formatter."""
    if config.LOG_FORMAT == "json":
        # Override existing configuration with predefined formatter.
        return formatters.JsonLogFormatter()

    if "format" in logger_info:
        log_format = logger_info["format"]
    else:
        log_format = _DEFAULT_FORMAT
    return formatters.TimestampFormatter(fmt=log_format)


def _shorten(logger_name):
    """Modify logger name."""
    # TODO: this function is only called for the fixture and fixture node loggers.
    #  If additional abbreviation is desired, we will need to call _shorten() in the appropriate places.

    removes = config.SHORTEN_LOGGER_NAME_CONFIG.get("remove", [])
    for remove in removes:
        logger_name = logger_name.replace(remove, "")

    replaces = config.SHORTEN_LOGGER_NAME_CONFIG.get("replace", {})
    for key, value in replaces.items():
        logger_name = logger_name.replace(key, value)

    # remove leading and trailing colons and underscores
    logger_name = re.sub(r"(^[:_]+|[:_]+$)", "", logger_name)

    return logger_name


# Utility functions for Evergreen file system logging.
# See `https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Task-Output-Directory#test-logs`
# for more information.


def _write_evergreen_log_spec():
    """Configure file system logging for Evergreen tasks."""
    if not config.EVERGREEN_WORK_DIR:
        return

    fp = f"{_get_evergreen_log_dirname()}/log_spec.yaml"
    os.makedirs(os.path.dirname(fp), exist_ok=True)

    ROOT_EXECUTOR_LOGGER.info("Writing Evergreen test log spec to %s.", fp)

    log_spec = {
        "version": 0,
        "format": "text-timestamp",
    }
    with open(fp, "w") as f:
        yaml.dump(log_spec, f)


def _add_evergreen_handler(logger, job_num, test_id=None, test_name=None):
    """Add a new evergreen handler to a logger."""
    logger_info = config.LOGGING_CONFIG[TESTS_LOGGER_NAME]
    evergreen_handler_info = None
    for handler_info in logger_info["handlers"]:
        if handler_info["class"] == "evergreen":
            evergreen_handler_info = handler_info
            break

    if evergreen_handler_info:
        fp = f"{_get_evergreen_log_dirname()}/{get_evergreen_log_name(job_num, test_id)}"
        os.makedirs(os.path.dirname(fp), exist_ok=True)

        handler = BufferedFileHandler(fp)
        if config.LOG_FORMAT == "json":
            handler.setFormatter(formatters.EvergreenJsonLogFormatter())
        else:
            handler.setFormatter(
                formatters.EvergreenLogFormatter(fmt=logger_info.get("format", _DEFAULT_FORMAT))
            )
        logger.addHandler(handler)

        if test_id:
            raw_url = RAW_TEST_LOGS_URL.format(
                task_id=config.EVERGREEN_TASK_ID,
                job_num=job_num,
                test_id=test_id,
                execution=config.EVERGREEN_EXECUTION,
            )
            ROOT_EXECUTOR_LOGGER.info("Writing output of %s to %s.", test_id, fp)
            ROOT_EXECUTOR_LOGGER.info("Raw logs for %s can be viewed at %s", test_name, raw_url)
        else:
            parsley_url = PARSLEY_JOBS_LOGS_URL.format(
                task_id=config.EVERGREEN_TASK_ID,
                job_num=job_num,
                execution=config.EVERGREEN_EXECUTION,
            )
            raw_url = RAW_JOBS_LOGS_URL.format(
                task_id=config.EVERGREEN_TASK_ID,
                job_num=job_num,
                execution=config.EVERGREEN_EXECUTION,
            )
            ROOT_EXECUTOR_LOGGER.info("Writing output of job #%d to %s.", job_num, fp)
            ROOT_EXECUTOR_LOGGER.info(
                "Parsley logs for job #%s can be viewed at %s", job_num, parsley_url
            )
            ROOT_EXECUTOR_LOGGER.info("Raw logs for job #%s can be viewed at %s", job_num, raw_url)


def _get_evergreen_log_dirname():
    """Return the reserved directory for test logs on the Evergreen task host."""
    return f"{config.EVERGREEN_WORK_DIR}/build/TestLogs"
