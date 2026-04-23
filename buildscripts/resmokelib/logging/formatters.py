"""Custom formatters for the logging handlers."""

import json
import logging
import re
import time

from buildscripts.resmokelib import config


class TimestampFormatter(logging.Formatter):
    """Timestamp formatter for log messages.

    Depending on whether --logFormat parameter is set to 'json' or 'plain'
    the timestamp format would be either 2021-01-01T13:27:03.246+00:00 or
    2021-01-01T13:27:03.246Z. This is done to match the format of timestamps
    produced by mongo{d,s} instances in case of 'json' log format but also to
    preserve the format used before this flag was introduced.

    TODO SERVER-99797: keep only +00:00 timestamp format after moving to json
    as the default --logFormat parameter.

    Note that +00:00 (UTC) time is used by default.
    """

    def formatTime(self, record, datefmt=None):
        """Return formatted time."""

        if datefmt is not None:
            # allow overrides, simply defer to super
            return super().formatTime(record, datefmt)

        # otherwise use very specific and terse defaults
        gm_time = time.gmtime(record.created)
        formatted_time = time.strftime("%Y-%m-%dT%H:%M:%S", gm_time)

        if config.LOG_FORMAT == "json":
            return "%s.%03d+00:00" % (formatted_time, record.msecs)
        else:
            return "%s.%03dZ" % (formatted_time, record.msecs)


class JsonLogFormatter(TimestampFormatter):
    """Log formatter that presents every line in a json log format.

    See `https://www.mongodb.com/docs/manual/reference/log-messages/#json-log-output-format`.
    """

    # When mongo shell forwards log lines from the spawned processes, it
    # prefixes every line with some process information. For instance:
    #
    # `d12345| {"t": ...}`
    #
    # The `prefixed_json_output` regex us used to separate `d12345|` part from
    # the json. `d12345` and the JSON are going to the groups #1 and #2
    # respectively.
    prefixed_json_output_re = re.compile(r"^([a-z]+\d+)\| (.+)$")

    def format(self, record):
        json_msg = None
        raw_message = record.getMessage()
        prefix = None

        # check for prefixed json first
        match = self.prefixed_json_output_re.match(raw_message)
        if match is not None:
            prefix = match.group(1)
            raw_message = match.group(2)

        try:
            json_msg = json.loads(raw_message)
        except json.JSONDecodeError:
            pass

        # Not a json dict, put original message into json.msg
        if not isinstance(json_msg, dict):
            json_msg = {
                "t": {"$date": self.formatTime(record)},
                "logger": record.name,
                "s": record.levelname[0],
                "c": "RESMOKE",
                "msg": raw_message,
            }

        if "logger" not in json_msg:
            json_msg["logger"] = record.name

        # If there is a process prefix, add it at the end of the logger name.
        if prefix is not None:
            json_msg["logger"] += ":" + prefix

        # Try to preserve order of keys in the dictionary across different log messages.
        # The desired order is ["t", "logger", "s", "c", "ctx", "msg", everything else]
        def key_sorter(pair):
            (k, _) = pair
            known_keys = ["t", "logger", "s", "c", "ctx", "msg"]
            order = dict(zip(known_keys, range(len(known_keys))))
            if k in known_keys:
                return order[k]
            return len(known_keys)

        formatted_message = dict(sorted(json_msg.items(), key=key_sorter))

        return json.dumps(formatted_message)


class EvergreenLogFormatter(TimestampFormatter):
    """Log line formatter for Evergreen log messages.

    See `https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Task-Output-Directory#test-logs`
    for more info.
    """

    def format(self, record):
        ts = int(record.created * 1e9)

        return "\n".join([f"{ts} {line}" for line in super().format(record).split("\n")])


class EvergreenJsonLogFormatter(JsonLogFormatter):
    """Json Log line formatter for Evergreen log messages.

    This is the 'JSON' version of EvergreenLogFormatter."""

    def format(self, record):
        ts = int(record.created * 1e9)

        # Prepend a timestamp to every line as required by evergreen, however, use the
        # 'JsonLogFormatter' to format the records instead of 'logging.Formatter'.
        return "\n".join([f"{ts} {line}" for line in super().format(record).split("\n")])
