"""Custom formatters for the logging handlers."""

import logging
import time


class ISO8601Formatter(logging.Formatter):
    """An ISO 8601 compliant formatter for log messages.

    It formats the timezone as an hour/minute offset and uses a period as the
    millisecond separator in order to match the log messages of MongoDB.
    """

    def formatTime(self, record, datefmt=None):
        """Return formatted time."""
        converted_time = self.converter(record.created)

        if datefmt is not None:
            return time.strftime(datefmt, converted_time)

        formatted_time = time.strftime("%Y-%m-%dT%H:%M:%S", converted_time)
        timezone = ISO8601Formatter._format_timezone_offset(converted_time)
        return "%s.%03d%s" % (formatted_time, record.msecs, timezone)

    @staticmethod
    def _format_timezone_offset(converted_time):
        """Return the timezone as an hour/minute offset in the form "+HHMM" or "-HHMM"."""

        # Windows treats %z in the format string as %Z, so we compute the hour/minute offset
        # manually.
        if converted_time.tm_isdst == 1 and time.daylight:
            utc_offset_secs = time.altzone
        else:
            utc_offset_secs = time.timezone

        # The offset is positive if the local timezone is behind (east of) UTC, and negative if it
        # is ahead (west) of UTC.
        utc_offset_prefix = "-" if utc_offset_secs > 0 else "+"
        utc_offset_secs = abs(utc_offset_secs)

        utc_offset_mins = (utc_offset_secs / 60) % 60
        utc_offset_hours = utc_offset_secs / 3600
        return "%s%02d%02d" % (utc_offset_prefix, utc_offset_hours, utc_offset_mins)


class EvergreenLogFormatter(logging.Formatter):
    """Log line formatter for Evergreen log messages.

    See `https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Task-Output-Directory#test-logs`
    for more info.
    """

    def format(self, record):
        """Return the Evergreen formatted record."""
        ts = int(record.created * 1e9)

        return "\n".join([f"{ts} {line}" for line in super().format(record).split("\n")])
