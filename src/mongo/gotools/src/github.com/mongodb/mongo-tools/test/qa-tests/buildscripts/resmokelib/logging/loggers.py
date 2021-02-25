"""
Module to hold the logger instances themselves.
"""

from __future__ import absolute_import

import logging

EXECUTOR_LOGGER_NAME = "executor"
FIXTURE_LOGGER_NAME = "fixture"
TESTS_LOGGER_NAME = "tests"

def new_logger(logger_name, parent=None):
    """
    Returns a new logging.Logger instance with the specified name.
    """

    # Set up the logger to handle all messages it receives.
    logger = logging.Logger(logger_name, level=logging.DEBUG)

    if parent is not None:
        logger.parent = parent
        logger.propagate = True

    return logger

EXECUTOR = new_logger(EXECUTOR_LOGGER_NAME)
FIXTURE = new_logger(FIXTURE_LOGGER_NAME)
TESTS = new_logger(TESTS_LOGGER_NAME)

LOGGERS_BY_NAME = {
    EXECUTOR_LOGGER_NAME: EXECUTOR,
    FIXTURE_LOGGER_NAME: FIXTURE,
    TESTS_LOGGER_NAME: TESTS,
}

_BUILDLOGGER_FALLBACK = new_logger("fallback")
