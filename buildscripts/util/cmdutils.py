"""Utilities for build commandline applications."""

import logging
import sys

import structlog
from structlog.dev import ConsoleRenderer, RichTracebackFormatter

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

    # structlog's default console renderer shows locals in exception tracebacks,
    # which is noisy and can leak secrets. This configures a new processor with
    # a renderer that doesn't show locals.
    processors = [
        p for p in structlog.get_config()["processors"] if not isinstance(p, ConsoleRenderer)
    ]
    processors.append(
        ConsoleRenderer(exception_formatter=RichTracebackFormatter(show_locals=False))
    )

    structlog.configure(processors=processors, logger_factory=structlog.stdlib.LoggerFactory())
    for log_name in EXTERNAL_LOGGERS:
        logging.getLogger(log_name).setLevel(logging.WARNING)
