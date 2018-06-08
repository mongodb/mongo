"""Module to hold the logger instances themselves."""

from __future__ import absolute_import

import logging
import sys

from . import buildlogger
from . import formatters
from .. import errors

_DEFAULT_FORMAT = "[%(name)s] %(message)s"

EXECUTOR_LOGGER_NAME = "executor"
FIXTURE_LOGGER_NAME = "fixture"
TESTS_LOGGER_NAME = "tests"

EXECUTOR_LOGGER = None


def _build_logger_server(logging_config):
    """Create and return a new BuildloggerServer.

    This occurs if "buildlogger" is configured as one of the handler class in the configuration,
    return None otherwise.
    """
    for logger_name in (FIXTURE_LOGGER_NAME, TESTS_LOGGER_NAME):
        logger_info = logging_config[logger_name]
        for handler_info in logger_info["handlers"]:
            if handler_info["class"] == "buildlogger":
                return buildlogger.BuildloggerServer()
    return None


def configure_loggers(logging_config):
    """Configure the loggers."""
    buildlogger.BUILDLOGGER_FALLBACK = BaseLogger("buildlogger")
    # The 'buildlogger' prefix is not added to the fallback logger since the prefix of the original
    # logger will be there as part of the logged message.
    buildlogger.BUILDLOGGER_FALLBACK.addHandler(
        _fallback_buildlogger_handler(include_logger_name=False))
    build_logger_server = _build_logger_server(logging_config)
    fixture_logger = FixtureRootLogger(logging_config, build_logger_server)
    tests_logger = TestsRootLogger(logging_config, build_logger_server)
    global EXECUTOR_LOGGER  # pylint: disable=global-statement
    EXECUTOR_LOGGER = ExecutorRootLogger(logging_config, build_logger_server, fixture_logger,
                                         tests_logger)


class BaseLogger(logging.Logger):
    """Base class for the custom loggers used in this library.

    Custom loggers share access to the logging configuration and provide methods
    to create other loggers.
    """

    def __init__(self, name, logging_config=None, build_logger_server=None, parent=None):
        """Initialize a BaseLogger.

        :param name: the logger name.
        :param logging_config: the logging configuration.
        :param build_logger_server: the build logger server (e.g. logkeeper).
        :param parent: the parent logger.
        """
        logging.Logger.__init__(self, name, level=logging.DEBUG)
        self._logging_config = logging_config
        self._build_logger_server = build_logger_server
        if parent:
            self.parent = parent
            self.propagate = True

    @property
    def build_logger_server(self):
        """Get the configured BuildloggerServer instance, or None."""
        if self._build_logger_server:
            return self._build_logger_server
        elif self.parent:
            # Fetching the value from parent
            return getattr(self.parent, "build_logger_server", None)
        return None

    @property
    def logging_config(self):
        """Get the logging configuration."""
        if self._logging_config:
            return self._logging_config
        elif self.parent:
            # Fetching the value from parent
            return getattr(self.parent, "logging_config", None)
        return None

    @staticmethod
    def get_formatter(logger_info):
        """Return formatter."""
        log_format = logger_info.get("format", _DEFAULT_FORMAT)
        return formatters.ISO8601Formatter(fmt=log_format)


class RootLogger(BaseLogger):
    """A custom class for top-level loggers (executor, fixture, tests)."""

    def __init__(self, name, logging_config, build_logger_server):
        """Initialize a RootLogger.

        :param name: the logger name.
        :param logging_config: the logging configuration.
        :param build_logger_server: the build logger server, if one is configured.
        """
        BaseLogger.__init__(self, name, logging_config, build_logger_server)
        self._configure()

    def _configure(self):
        if self.name not in self.logging_config:
            raise ValueError("Logging configuration should contain the %s component" % self.name)
        logger_info = self.logging_config[self.name]
        formatter = self.get_formatter(logger_info)

        for handler_info in logger_info.get("handlers", []):
            self._add_handler(handler_info, formatter)

    def _add_handler(self, handler_info, formatter):
        handler_class = handler_info["class"]
        if handler_class == "logging.FileHandler":
            handler = logging.FileHandler(filename=handler_info["filename"], mode=handler_info.get(
                "mode", "w"))
        elif handler_class == "logging.NullHandler":
            handler = logging.NullHandler()
        elif handler_class == "logging.StreamHandler":
            handler = logging.StreamHandler(sys.stdout)
        elif handler_class == "buildlogger":
            return  # Buildlogger handlers are applied when creating specific child loggers
        else:
            raise ValueError("Unknown handler class '%s'" % handler_class)
        handler.setFormatter(formatter)
        self.addHandler(handler)


class ExecutorRootLogger(RootLogger):
    """Class for the "executor" top-level logger."""

    def __init__(self, logging_config, build_logger_server, fixture_root_logger, tests_root_logger):
        """Initialize an ExecutorRootLogger."""
        RootLogger.__init__(self, EXECUTOR_LOGGER_NAME, logging_config, build_logger_server)
        self.fixture_root_logger = fixture_root_logger
        self.tests_root_logger = tests_root_logger

    def new_resmoke_logger(self):
        """Create a child logger of this logger with the name "resmoke"."""
        return BaseLogger("resmoke", parent=self)

    def new_job_logger(self, test_kind, job_num):
        """Create a new child JobLogger."""
        return JobLogger(test_kind, job_num, self, self.fixture_root_logger)

    def new_testqueue_logger(self, test_kind):
        """Create a new TestQueueLogger that will be a child of the "tests" root logger."""
        return TestQueueLogger(test_kind, self.tests_root_logger)

    def new_hook_logger(self, hook_class, fixture_logger):
        """Create a new child hook logger."""
        return HookLogger(hook_class, fixture_logger, self.tests_root_logger)


class JobLogger(BaseLogger):
    """JobLogger class."""

    def __init__(self, test_kind, job_num, parent, fixture_root_logger):
        """Initialize a JobLogger.

        :param test_kind: the test kind (e.g. js_test, db_test, etc.).
        :param job_num: a job number.
        :param fixture_root_logger: the root logger for the fixture logs.
        """
        name = "executor:%s:job%d" % (test_kind, job_num)
        BaseLogger.__init__(self, name, parent=parent)
        self.job_num = job_num
        self.fixture_root_logger = fixture_root_logger
        if self.build_logger_server:
            # If we're configured to log messages to the buildlogger server, then request a new
            # build_id for this job.
            self.build_id = self.build_logger_server.new_build_id("job%d" % job_num)
            if not self.build_id:
                buildlogger.set_log_output_incomplete()
                raise errors.LoggerRuntimeConfigError(
                    "Encountered an error configuring buildlogger for job #{:d}: Failed to get a"
                    " new build_id".format(job_num))

            url = self.build_logger_server.get_build_log_url(self.build_id)
            parent.info("Writing output of job #%d to %s.", job_num, url)
        else:
            self.build_id = None

    def new_fixture_logger(self, fixture_class):
        """Create a new fixture logger that will be a child of the "fixture" root logger."""
        return FixtureLogger(fixture_class, self.job_num, self.build_id, self.fixture_root_logger)

    def new_test_logger(self, test_shortname, test_basename, command, parent):
        """Create a new test logger that will be a child of the given parent."""
        if self.build_id:
            # If we're configured to log messages to the buildlogger server, then request a new
            # test_id for this test.
            test_id = self.build_logger_server.new_test_id(self.build_id, test_basename, command)
            if not test_id:
                buildlogger.set_log_output_incomplete()
                raise errors.LoggerRuntimeConfigError(
                    "Encountered an error configuring buildlogger for test {}: Failed to get a new"
                    " test_id".format(test_basename))

            url = self.build_logger_server.get_test_log_url(self.build_id, test_id)
            self.info("Writing output of %s to %s.", test_basename, url)
            return TestLogger(test_shortname, parent, self.build_id, test_id, url)

        return TestLogger(test_shortname, parent)


class TestLogger(BaseLogger):
    """TestLogger class."""

    def __init__(  # pylint: disable=too-many-arguments
            self, test_name, parent, build_id=None, test_id=None, url=None):
        """Initialize a TestLogger.

        :param test_name: the test name.
        :param parent: the parent logger.
        :param build_id: the build logger build id.
        :param test_id: the build logger test id.
        :param url: the build logger URL endpoint for the test.
        """
        name = "%s:%s" % (parent.name, test_name)
        BaseLogger.__init__(self, name, parent=parent)
        self.url_endpoint = url
        self._add_build_logger_handler(build_id, test_id)

    def _add_build_logger_handler(self, build_id, test_id):
        logger_info = self.logging_config[TESTS_LOGGER_NAME]
        handler_info = _get_buildlogger_handler_info(logger_info)
        if handler_info is not None:
            handler = self.build_logger_server.get_test_handler(build_id, test_id, handler_info)
            handler.setFormatter(self.get_formatter(logger_info))
            self.addHandler(handler)

    def new_test_thread_logger(self, test_kind, thread_id):
        """Create a new child test thread logger."""
        return BaseLogger("%s:%s" % (test_kind, thread_id), parent=self)


class FixtureRootLogger(RootLogger):
    """Class for the "fixture" top-level logger."""

    def __init__(self, logging_config, build_logger_server):
        """Initialize a FixtureRootLogger.

        :param logging_config: the logging configuration.
        :param build_logger_server: the build logger server, if one is configured.
        """
        RootLogger.__init__(self, FIXTURE_LOGGER_NAME, logging_config, build_logger_server)


class FixtureLogger(BaseLogger):
    """FixtureLogger class."""

    def __init__(self, fixture_class, job_num, build_id, fixture_root_logger):
        """Initialize a FixtureLogger.

        :param fixture_class: the name of the fixture class.
        :param job_num: the number of the job the fixture is running on.
        :param build_id: the build logger build id, if any.
        :param fixture_root_logger: the root logger for the fixture logs.
        """
        BaseLogger.__init__(self, "%s:job%d" % (fixture_class, job_num), parent=fixture_root_logger)
        self.fixture_class = fixture_class
        self.job_num = job_num
        self._add_build_logger_handler(build_id)

    def _add_build_logger_handler(self, build_id):
        logger_info = self.logging_config[FIXTURE_LOGGER_NAME]
        handler_info = _get_buildlogger_handler_info(logger_info)
        if handler_info is not None:
            handler = self.build_logger_server.get_global_handler(build_id, handler_info)
            handler.setFormatter(self.get_formatter(logger_info))
            self.addHandler(handler)

    def new_fixture_node_logger(self, node_name):
        """Create a new child FixtureNodeLogger."""
        return FixtureNodeLogger(self.fixture_class, self.job_num, node_name, self)


class FixtureNodeLogger(BaseLogger):
    """FixtureNodeLogger class."""

    def __init__(self, fixture_class, job_num, node_name, fixture_logger):
        """Initialize a FixtureNodeLogger.

        :param fixture_class: the name of the fixture implementation class.
        :param job_num: the number of the job the fixture is running on.
        :param node_name: the node display name.
        :param fixture_logger: the parent fixture logger.
        """
        BaseLogger.__init__(self, "%s:job%d:%s" % (fixture_class, job_num, node_name),
                            parent=fixture_logger)
        self.fixture_class = fixture_class
        self.job_num = job_num
        self.node_name = node_name

    def new_fixture_node_logger(self, node_name):
        """Create a new child FixtureNodeLogger."""
        return FixtureNodeLogger(self.fixture_class, self.job_num, "%s:%s" % (self.node_name,
                                                                              node_name), self)


class TestsRootLogger(RootLogger):
    """Class for the "tests" top-level logger."""

    def __init__(self, logging_config, build_logger_server):
        """Initialize a TestsRootLogger.

        :param logging_config: the logging configuration.
        :param build_logger_server: the build logger server, if one is configured.
        """
        RootLogger.__init__(self, TESTS_LOGGER_NAME, logging_config, build_logger_server)


class TestQueueLogger(BaseLogger):
    """TestQueueLogger class."""

    def __init__(self, test_kind, tests_root_logger):
        """Initialize a TestQueueLogger.

        :param test_kind: the test kind (e.g. js_test, db_test, cpp_unit_test, etc.).
        :param tests_root_logger: the root logger for the tests logs.
        """
        BaseLogger.__init__(self, test_kind, parent=tests_root_logger)


class HookLogger(BaseLogger):
    """HookLogger class."""

    def __init__(self, hook_class, fixture_logger, tests_root_logger):
        """Initialize a HookLogger.

        :param hook_class: the hook's name (e.g. CheckReplDBHash, ValidateCollections, etc.).
        :param fixture_logger: the logger for the fixtures logs.
        :param tests_root_logger: the root logger for the tests logs.
        """
        logger_name = "{}:job{:d}".format(hook_class, fixture_logger.job_num)
        BaseLogger.__init__(self, logger_name, parent=fixture_logger)

        self.test_case_logger = BaseLogger(logger_name, parent=tests_root_logger)


# Util methods


def _fallback_buildlogger_handler(include_logger_name=True):
    """Return a handler that writes to stderr."""
    if include_logger_name:
        log_format = "[fallback] [%(name)s] %(message)s"
    else:
        log_format = "[fallback] %(message)s"
    formatter = formatters.ISO8601Formatter(fmt=log_format)

    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(formatter)

    return handler


def _get_buildlogger_handler_info(logger_info):
    """Return the buildlogger handler information if it exists, and None otherwise."""
    for handler_info in logger_info["handlers"]:
        handler_info = handler_info.copy()
        if handler_info.pop("class") == "buildlogger":
            return handler_info
    return None
