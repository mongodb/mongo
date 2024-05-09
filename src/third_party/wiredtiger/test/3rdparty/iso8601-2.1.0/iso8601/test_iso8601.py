# coding=UTF-8
from __future__ import absolute_import

import copy
import datetime
import pickle

import hypothesis
import hypothesis.extra.pytz
import hypothesis.strategies
import pytest

from . import iso8601


def test_iso8601_regex() -> None:
    assert iso8601.ISO8601_REGEX.match("2006-10-11T00:14:33Z")


def test_fixedoffset_eq() -> None:
    # See https://bitbucket.org/micktwomey/pyiso8601/issues/19
    expected_timezone = datetime.timezone(offset=datetime.timedelta(hours=2))
    assert expected_timezone == iso8601.FixedOffset(2, 0, "+2:00")


def test_parse_no_timezone_different_default() -> None:
    tz = iso8601.FixedOffset(2, 0, "test offset")
    d = iso8601.parse_date("2007-01-01T08:00:00", default_timezone=tz)
    assert d == datetime.datetime(2007, 1, 1, 8, 0, 0, 0, tz)
    assert d.tzinfo == tz


def test_parse_utc_different_default() -> None:
    """Z should mean 'UTC', not 'default'."""
    tz = iso8601.FixedOffset(2, 0, "test offset")
    d = iso8601.parse_date("2007-01-01T08:00:00Z", default_timezone=tz)
    assert d == datetime.datetime(2007, 1, 1, 8, 0, 0, 0, iso8601.UTC)


@pytest.mark.parametrize(
    "invalid_date, error_string",
    [
        ("2013-10-", "Unable to parse date string"),
        ("2013-", "Unable to parse date string"),
        ("", "Unable to parse date string"),
        ("wibble", "Unable to parse date string"),
        ("23", "Unable to parse date string"),
        ("131015T142533Z", "Unable to parse date string"),
        ("131015", "Unable to parse date string"),
        ("20141", "Unable to parse date string"),
        ("201402", "Unable to parse date string"),
        (
            "2007-06-23X06:40:34.00Z",
            "Unable to parse date string",
        ),  # https://code.google.com/p/pyiso8601/issues/detail?id=14
        (
            "2007-06-23 06:40:34.00Zrubbish",
            "Unable to parse date string",
        ),  # https://code.google.com/p/pyiso8601/issues/detail?id=14
        ("20114-01-03T01:45:49", "Unable to parse date string"),
    ],
)
def test_parse_invalid_date(invalid_date: str, error_string: str) -> None:
    assert iso8601.is_iso8601(invalid_date) is False
    with pytest.raises(iso8601.ParseError) as exc:
        iso8601.parse_date(invalid_date)
    assert exc.errisinstance(iso8601.ParseError)
    assert str(exc.value).startswith(error_string)


@pytest.mark.parametrize(
    "valid_date,expected_datetime,isoformat",
    [
        (
            "2007-06-23 06:40:34.00Z",
            datetime.datetime(2007, 6, 23, 6, 40, 34, 0, iso8601.UTC),
            "2007-06-23T06:40:34+00:00",
        ),  # Handle a separator other than T
        (
            "1997-07-16T19:20+01:00",
            datetime.datetime(
                1997, 7, 16, 19, 20, 0, 0, iso8601.FixedOffset(1, 0, "+01:00")
            ),
            "1997-07-16T19:20:00+01:00",
        ),  # Parse with no seconds
        (
            "2007-01-01T08:00:00",
            datetime.datetime(2007, 1, 1, 8, 0, 0, 0, iso8601.UTC),
            "2007-01-01T08:00:00+00:00",
        ),  # Handle timezone-less dates. Assumes UTC. http://code.google.com/p/pyiso8601/issues/detail?id=4
        (
            "2006-10-20T15:34:56.123+02:30",
            datetime.datetime(
                2006, 10, 20, 15, 34, 56, 123000, iso8601.FixedOffset(2, 30, "+02:30")
            ),
            None,
        ),
        (
            "2006-10-20T15:34:56Z",
            datetime.datetime(2006, 10, 20, 15, 34, 56, 0, iso8601.UTC),
            "2006-10-20T15:34:56+00:00",
        ),
        (
            "2007-5-7T11:43:55.328Z",
            datetime.datetime(2007, 5, 7, 11, 43, 55, 328000, iso8601.UTC),
            "2007-05-07T11:43:55.328000+00:00",
        ),  # http://code.google.com/p/pyiso8601/issues/detail?id=6
        (
            "2006-10-20T15:34:56.123Z",
            datetime.datetime(2006, 10, 20, 15, 34, 56, 123000, iso8601.UTC),
            "2006-10-20T15:34:56.123000+00:00",
        ),
        (
            "2013-10-15T18:30Z",
            datetime.datetime(2013, 10, 15, 18, 30, 0, 0, iso8601.UTC),
            "2013-10-15T18:30:00+00:00",
        ),
        (
            "2013-10-15T22:30+04",
            datetime.datetime(
                2013, 10, 15, 22, 30, 0, 0, iso8601.FixedOffset(4, 0, "+04:00")
            ),
            "2013-10-15T22:30:00+04:00",
        ),  # <time>±hh:mm
        (
            "2013-10-15T1130-0700",
            datetime.datetime(
                2013, 10, 15, 11, 30, 0, 0, iso8601.FixedOffset(-7, 0, "-07:00")
            ),
            "2013-10-15T11:30:00-07:00",
        ),  # <time>±hhmm
        (
            "2013-10-15T1130+0700",
            datetime.datetime(
                2013, 10, 15, 11, 30, 0, 0, iso8601.FixedOffset(+7, 0, "+07:00")
            ),
            "2013-10-15T11:30:00+07:00",
        ),  # <time>±hhmm
        (
            "2013-10-15T1130+07",
            datetime.datetime(
                2013, 10, 15, 11, 30, 0, 0, iso8601.FixedOffset(+7, 0, "+07:00")
            ),
            "2013-10-15T11:30:00+07:00",
        ),  # <time>±hh
        (
            "2013-10-15T1130-07",
            datetime.datetime(
                2013, 10, 15, 11, 30, 0, 0, iso8601.FixedOffset(-7, 0, "-07:00")
            ),
            "2013-10-15T11:30:00-07:00",
        ),  # <time>±hh
        (
            "2013-10-15T15:00-03:30",
            datetime.datetime(
                2013, 10, 15, 15, 0, 0, 0, iso8601.FixedOffset(-3, -30, "-03:30")
            ),
            "2013-10-15T15:00:00-03:30",
        ),
        (
            "2013-10-15T183123Z",
            datetime.datetime(2013, 10, 15, 18, 31, 23, 0, iso8601.UTC),
            "2013-10-15T18:31:23+00:00",
        ),  # hhmmss
        (
            "2013-10-15T1831Z",
            datetime.datetime(2013, 10, 15, 18, 31, 0, 0, iso8601.UTC),
            "2013-10-15T18:31:00+00:00",
        ),  # hhmm
        (
            "2013-10-15T18Z",
            datetime.datetime(2013, 10, 15, 18, 0, 0, 0, iso8601.UTC),
            "2013-10-15T18:00:00+00:00",
        ),  # hh
        (
            "2013-10-15",
            datetime.datetime(2013, 10, 15, 0, 0, 0, 0, iso8601.UTC),
            "2013-10-15T00:00:00+00:00",
        ),  # YYYY-MM-DD
        (
            "20131015T18:30Z",
            datetime.datetime(2013, 10, 15, 18, 30, 0, 0, iso8601.UTC),
            "2013-10-15T18:30:00+00:00",
        ),  # YYYYMMDD
        (
            "2012-12-19T23:21:28.512400+00:00",
            datetime.datetime(
                2012, 12, 19, 23, 21, 28, 512400, iso8601.FixedOffset(0, 0, "+00:00")
            ),
            "2012-12-19T23:21:28.512400+00:00",
        ),  # https://code.google.com/p/pyiso8601/issues/detail?id=21
        (
            "2006-10-20T15:34:56.123+0230",
            datetime.datetime(
                2006, 10, 20, 15, 34, 56, 123000, iso8601.FixedOffset(2, 30, "+02:30")
            ),
            "2006-10-20T15:34:56.123000+02:30",
        ),  # https://code.google.com/p/pyiso8601/issues/detail?id=18
        (
            "19950204",
            datetime.datetime(1995, 2, 4, tzinfo=iso8601.UTC),
            "1995-02-04T00:00:00+00:00",
        ),  # https://code.google.com/p/pyiso8601/issues/detail?id=1
        (
            "2010-07-20 15:25:52.520701+00:00",
            datetime.datetime(
                2010, 7, 20, 15, 25, 52, 520701, iso8601.FixedOffset(0, 0, "+00:00")
            ),
            "2010-07-20T15:25:52.520701+00:00",
        ),  # https://code.google.com/p/pyiso8601/issues/detail?id=17
        (
            "2010-06-12",
            datetime.datetime(2010, 6, 12, tzinfo=iso8601.UTC),
            "2010-06-12T00:00:00+00:00",
        ),  # https://code.google.com/p/pyiso8601/issues/detail?id=16
        (
            "1985-04-12T23:20:50.52-05:30",
            datetime.datetime(
                1985, 4, 12, 23, 20, 50, 520000, iso8601.FixedOffset(-5, -30, "-05:30")
            ),
            "1985-04-12T23:20:50.520000-05:30",
        ),  # https://bitbucket.org/micktwomey/pyiso8601/issue/8/015-parses-negative-timezones-incorrectly
        (
            "1997-08-29T06:14:00.000123Z",
            datetime.datetime(1997, 8, 29, 6, 14, 0, 123, iso8601.UTC),
            "1997-08-29T06:14:00.000123+00:00",
        ),  # https://bitbucket.org/micktwomey/pyiso8601/issue/9/regression-parsing-microseconds
        (
            "2014-02",
            datetime.datetime(2014, 2, 1, 0, 0, 0, 0, iso8601.UTC),
            "2014-02-01T00:00:00+00:00",
        ),  # https://bitbucket.org/micktwomey/pyiso8601/issue/14/regression-yyyy-mm-no-longer-parses
        (
            "2014",
            datetime.datetime(2014, 1, 1, 0, 0, 0, 0, iso8601.UTC),
            "2014-01-01T00:00:00+00:00",
        ),  # YYYY
        (
            "1997-08-29T06:14:00,000123Z",
            datetime.datetime(1997, 8, 29, 6, 14, 0, 123, iso8601.UTC),
            "1997-08-29T06:14:00.000123+00:00",
        ),  # Use , as decimal separator
    ],
)
def test_parse_valid_date(
    valid_date: str, expected_datetime: datetime.datetime, isoformat: str
) -> None:
    assert iso8601.is_iso8601(valid_date) is True
    parsed = iso8601.parse_date(valid_date)
    assert parsed.year == expected_datetime.year
    assert parsed.month == expected_datetime.month
    assert parsed.day == expected_datetime.day
    assert parsed.hour == expected_datetime.hour
    assert parsed.minute == expected_datetime.minute
    assert parsed.second == expected_datetime.second
    assert parsed.microsecond == expected_datetime.microsecond
    assert parsed.tzinfo == expected_datetime.tzinfo
    assert parsed == expected_datetime
    assert parsed.isoformat() == expected_datetime.isoformat()
    copy.deepcopy(parsed)  # ensure it's deep copy-able
    pickle.dumps(parsed)  # ensure it pickles
    if isoformat:
        assert parsed.isoformat() == isoformat
    assert iso8601.parse_date(parsed.isoformat()) == parsed  # Test round trip


@hypothesis.given(s=hypothesis.strategies.datetimes())
def test_hypothesis_valid_naive_datetimes(s: datetime.datetime) -> None:
    as_string = s.isoformat()
    parsed = iso8601.parse_date(as_string, default_timezone=None)
    print(f"{s!r} {as_string!r} {parsed!r}")
    assert s == parsed


@hypothesis.given(
    s=hypothesis.strategies.datetimes(timezones=hypothesis.extra.pytz.timezones())
)
def test_hypothesis_valid_datetimes_with_timezone(s: datetime.datetime) -> None:
    as_string = s.isoformat()
    parsed = iso8601.parse_date(as_string)
    print(f"{s!r} {as_string!r} {parsed!r}")
    assert s == parsed
