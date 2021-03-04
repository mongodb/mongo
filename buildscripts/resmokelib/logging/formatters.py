"""Custom formatters for the logging handlers."""

import logging
import time


class TimestampFormatter(logging.Formatter):
    """Timestamp formatter for log messages.

    Timestamp format example: 13:27:03.246Z
    """

    def formatTime(self, record, datefmt=None):
        """Return formatted time."""
        converted_time = self.converter(record.created)

        if datefmt is not None:
            return time.strftime(datefmt, converted_time)

        formatted_time = time.strftime("%H:%M:%S", converted_time)
        return "%s.%03dZ" % (formatted_time, record.msecs)
