"""ISO 8601 date time string parsing

Basic usage:
>>> import iso8601
>>> iso8601.parse_date("2007-01-25T12:00:00Z")
datetime.datetime(2007, 1, 25, 12, 0, tzinfo=<iso8601.Utc ...>)
>>>

"""

import datetime
import re
import typing
from decimal import Decimal

__all__ = ["parse_date", "ParseError", "UTC", "FixedOffset"]

# Adapted from http://delete.me.uk/2005/03/iso8601.html
ISO8601_REGEX = re.compile(
    r"""
    (?P<year>[0-9]{4})
    (
        (
            (-(?P<monthdash>[0-9]{1,2}))
            |
            (?P<month>[0-9]{2})
            (?!$)  # Don't allow YYYYMM
        )
        (
            (
                (-(?P<daydash>[0-9]{1,2}))
                |
                (?P<day>[0-9]{2})
            )
            (
                (
                    (?P<separator>[ T])
                    (?P<hour>[0-9]{2})
                    (:{0,1}(?P<minute>[0-9]{2})){0,1}
                    (
                        :{0,1}(?P<second>[0-9]{1,2})
                        ([.,](?P<second_fraction>[0-9]+)){0,1}
                    ){0,1}
                    (?P<timezone>
                        Z
                        |
                        (
                            (?P<tz_sign>[-+])
                            (?P<tz_hour>[0-9]{2})
                            :{0,1}
                            (?P<tz_minute>[0-9]{2}){0,1}
                        )
                    ){0,1}
                ){0,1}
            )
        ){0,1}  # YYYY-MM
    ){0,1}  # YYYY only
    $
    """,
    re.VERBOSE,
)


class ParseError(ValueError):
    """Raised when there is a problem parsing a date string"""


UTC = datetime.timezone.utc


def FixedOffset(
    offset_hours: float, offset_minutes: float, name: str
) -> datetime.timezone:
    return datetime.timezone(
        datetime.timedelta(hours=offset_hours, minutes=offset_minutes), name
    )


def parse_timezone(
    matches: typing.Dict[str, str],
    default_timezone: typing.Optional[datetime.timezone] = UTC,
) -> typing.Optional[datetime.timezone]:
    """Parses ISO 8601 time zone specs into tzinfo offsets"""
    tz = matches.get("timezone", None)
    if tz == "Z":
        return UTC
    # This isn't strictly correct, but it's common to encounter dates without
    # timezones so I'll assume the default (which defaults to UTC).
    # Addresses issue 4.
    if tz is None:
        return default_timezone
    sign = matches.get("tz_sign", None)
    hours = int(matches.get("tz_hour", 0))
    minutes = int(matches.get("tz_minute", 0))
    description = f"{sign}{hours:02d}:{minutes:02d}"
    if sign == "-":
        hours = -hours
        minutes = -minutes
    return FixedOffset(hours, minutes, description)


def parse_date(
    datestring: str, default_timezone: typing.Optional[datetime.timezone] = UTC
) -> datetime.datetime:
    """Parses ISO 8601 dates into datetime objects

    The timezone is parsed from the date string. However it is quite common to
    have dates without a timezone (not strictly correct). In this case the
    default timezone specified in default_timezone is used. This is UTC by
    default.

    :param datestring: The date to parse as a string
    :param default_timezone: A datetime tzinfo instance to use when no timezone
                             is specified in the datestring. If this is set to
                             None then a naive datetime object is returned.
    :returns: A datetime.datetime instance
    :raises: ParseError when there is a problem parsing the date or
             constructing the datetime instance.

    """
    try:
        m = ISO8601_REGEX.match(datestring)
    except Exception as e:
        raise ParseError(e)

    if not m:
        raise ParseError(f"Unable to parse date string {datestring!r}")

    # Drop any Nones from the regex matches
    # TODO: check if there's a way to omit results in regexes
    groups: typing.Dict[str, str] = {
        k: v for k, v in m.groupdict().items() if v is not None
    }

    try:
        return datetime.datetime(
            year=int(groups.get("year", 0)),
            month=int(groups.get("month", groups.get("monthdash", 1))),
            day=int(groups.get("day", groups.get("daydash", 1))),
            hour=int(groups.get("hour", 0)),
            minute=int(groups.get("minute", 0)),
            second=int(groups.get("second", 0)),
            microsecond=int(
                Decimal(f"0.{groups.get('second_fraction', 0)}") * Decimal("1000000.0")
            ),
            tzinfo=parse_timezone(groups, default_timezone=default_timezone),
        )
    except Exception as e:
        raise ParseError(e)


def is_iso8601(datestring: str) -> bool:
    """Check if a string matches an ISO 8601 format.

    :param datestring: The string to check for validity
    :returns: True if the string matches an ISO 8601 format, False otherwise
    """
    try:
        m = ISO8601_REGEX.match(datestring)
        return bool(m)
    except Exception as e:
        raise ParseError(e)
