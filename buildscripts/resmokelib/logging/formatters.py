"""Custom formatters for the logging handlers."""

import logging
import time


class TimestampFormatter(logging.Formatter):
    """Timestamp formatter for log messages.

    Timestamp format example: 13:27:03.246Z
    Note that Zulu (UTC) time is used by default.
    """

    def formatTime(self, record, datefmt=None):
        """Return formatted time."""

        if datefmt is not None:
            # allow overrides, simply defer to super
            return super().formatTime(record, datefmt)

        # otherwise use very specific and terse defaults
        gm_time = time.gmtime(record.created)
        formatted_time = time.strftime("%H:%M:%S", gm_time)
        return "%s.%03dZ" % (formatted_time, record.msecs)


class EvergreenLogFormatter(logging.Formatter):
    """Log line formatter for Evergreen log messages.

    See `https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Task-Output-Directory#test-logs`
    for more info.
    """

    def format(self, record):
        ts = int(record.created * 1e9)

        return "\n".join([f"{ts} {line}" for line in super().format(record).split("\n")])
