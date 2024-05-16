"""Utilities for build commandline applications."""

import logging
import sys

import structlog

EXTERNAL_LOGGERS = {
    "evergreen",
    "git",
    "inject",
    "urllib3",
}


def enable_logging(verbose: bool) -> None:
    """
    Enable logging for execution.

    :param verbose: Should verbose logging be enabled.
    """
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=level,
        stream=sys.stdout,
    )
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())
    for log_name in EXTERNAL_LOGGERS:
        logging.getLogger(log_name).setLevel(logging.WARNING)
