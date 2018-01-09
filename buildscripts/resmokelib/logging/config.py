"""
Configuration functions for the logging package.
"""

from __future__ import absolute_import

import logging
import sys

from . import buildlogger
from . import formatters
from . import loggers


_DEFAULT_FORMAT = "[%(name)s] %(message)s"


def using_buildlogger(logging_config):
    """
    Returns true if buildlogger is set as a handler on the "fixture" or
    "tests" loggers, and false otherwise.
    """
    for logger_name in (loggers.FIXTURE_LOGGER_NAME, loggers.TESTS_LOGGER_NAME):
        logger_info = logging_config[logger_name]
        if _get_buildlogger_handler_info(logger_info) is not None:
            return True
    return False


def apply_config(logging_config):
    """
    Adds all handlers specified by the configuration to the "executor",
    "fixture", and "tests" loggers.
    """

    logging_components = (loggers.EXECUTOR_LOGGER_NAME,
                          loggers.FIXTURE_LOGGER_NAME,
                          loggers.TESTS_LOGGER_NAME)

    if not all(component in logging_config for component in logging_components):
        raise ValueError("Logging configuration should contain %s, %s, and %s components"
                         % logging_components)

    # Configure the executor, fixture, and tests loggers.
    for component in logging_components:
        logger = loggers.LOGGERS_BY_NAME[component]
        logger_info = logging_config[component]
        _configure_logger(logger, logger_info)

    # Configure the buildlogger logger.
    loggers._BUILDLOGGER_FALLBACK.addHandler(_fallback_buildlogger_handler())


def apply_buildlogger_global_handler(logger, logging_config, build_id=None, build_config=None):
    """
    Adds a buildlogger.BuildloggerGlobalHandler to 'logger' if specified
    to do so by the configuration.
    """

    logger_info = logging_config[loggers.FIXTURE_LOGGER_NAME]
    handler_info = _get_buildlogger_handler_info(logger_info)
    if handler_info is None:
        # Not configured to use buildlogger.
        return

    if all(x is not None for x in (build_id, build_config)):
        log_format = logger_info.get("format", _DEFAULT_FORMAT)
        formatter = formatters.ISO8601Formatter(fmt=log_format)

        handler = buildlogger.BuildloggerGlobalHandler(build_config,
                                                       build_id,
                                                       **handler_info)
        handler.setFormatter(formatter)
    else:
        handler = _fallback_buildlogger_handler()
        # Fallback handler already has formatting configured.

    logger.addHandler(handler)


def apply_buildlogger_test_handler(logger,
                                   logging_config,
                                   build_id=None,
                                   build_config=None,
                                   test_id=None):
    """
    Adds a buildlogger.BuildloggerTestHandler to 'logger' if specified
    to do so by the configuration.
    """

    logger_info = logging_config[loggers.TESTS_LOGGER_NAME]
    handler_info = _get_buildlogger_handler_info(logger_info)
    if handler_info is None:
        # Not configured to use buildlogger.
        return

    if all(x is not None for x in (build_id, build_config, test_id)):
        log_format = logger_info.get("format", _DEFAULT_FORMAT)
        formatter = formatters.ISO8601Formatter(fmt=log_format)

        handler = buildlogger.BuildloggerTestHandler(build_config,
                                                     build_id,
                                                     test_id,
                                                     **handler_info)
        handler.setFormatter(formatter)
    else:
        handler = _fallback_buildlogger_handler()
        # Fallback handler already has formatting configured.

    logger.addHandler(handler)


def _configure_logger(logger, logger_info):
    """
    Adds the handlers specified by the configuration to 'logger'.
    """

    log_format = logger_info.get("format", _DEFAULT_FORMAT)
    formatter = formatters.ISO8601Formatter(fmt=log_format)

    for handler_info in logger_info.get("handlers", []):
        handler_class = handler_info["class"]
        if handler_class == "logging.FileHandler":
            handler = logging.FileHandler(filename=handler_info["filename"],
                                          mode=handler_info.get("mode", "w"))
        elif handler_class == "logging.NullHandler":
            handler = logging.NullHandler()
        elif handler_class == "logging.StreamHandler":
            handler = logging.StreamHandler(sys.stdout)
        elif handler_class == "buildlogger":
            continue  # Buildlogger handlers are applied when running tests.
        else:
            raise ValueError("Unknown handler class '%s'" % (handler_class))
        handler.setFormatter(formatter)
        logger.addHandler(handler)


def _fallback_buildlogger_handler():
    """
    Returns a handler that writes to stderr.
    """

    log_format = "[buildlogger:%(name)s] %(message)s"
    formatter = formatters.ISO8601Formatter(fmt=log_format)

    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(formatter)

    return handler

def _get_buildlogger_handler_info(logger_info):
    """
    Returns the buildlogger handler information if it exists, and None
    otherwise.
    """

    for handler_info in logger_info["handlers"]:
        handler_info = handler_info.copy()
        if handler_info.pop("class") == "buildlogger":
            return handler_info
    return None
