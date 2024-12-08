"""Extension to the logging package to support different loggers."""

# Alias the built-in logging.Logger class for type checking arguments. Those interested in
# constructing a new Logger instance should use the loggers.new_logger() function instead.
from logging import Logger  # noqa: F401

from buildscripts.resmokelib.logging import flush, loggers

__all__ = ["flush", "loggers"]
