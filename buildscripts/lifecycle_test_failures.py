#!/usr/bin/env python
"""Utility for computing test failure rates from the Evergreen API."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import datetime
import itertools
import logging
import operator
import optparse
import os
import sys
import time

try:
    from urlparse import urlparse
except ImportError:
    from urllib.parse import urlparse  # type: ignore

import yaml

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.client import evergreen as evergreen  # pylint: disable=wrong-import-position

LOGGER = logging.getLogger(__name__)

if sys.version_info[0] == 2:
    _STRING_TYPES = (basestring, )
else:
    _STRING_TYPES = (str, )

_ReportEntry = collections.namedtuple("_ReportEntry", [
    "test",
    "task",
    "variant",
    "distro",
    "start_date",
    "end_date",
    "num_pass",
    "num_fail",
])


class Wildcard(object):
    """Class for representing there are multiple values associated with a particular component."""

    def __init__(self, kind):
        """Initialize Wildcard."""
        self._kind = kind

    def __eq__(self, other):
        if not isinstance(other, Wildcard):
            return NotImplemented

        return self._kind == other._kind  # pylint: disable=protected-access

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash(self._kind)

    def __str__(self):
        return "<multiple {}>".format(self._kind)


class ReportEntry(_ReportEntry):
    """Information about Evergreen test executions."""

    _MULTIPLE_TESTS = Wildcard("tests")
    _MULTIPLE_TASKS = Wildcard("tasks")
    _MULTIPLE_VARIANTS = Wildcard("variants")
    _MULTIPLE_DISTROS = Wildcard("distros")

    _MIN_DATE = datetime.date(datetime.MINYEAR, 1, 1)
    _MAX_DATE = datetime.date(datetime.MAXYEAR, 12, 31)

    @property
    def fail_rate(self):
        """Get the fraction of test failures to total number of test executions.

        If a test hasn't been run at all, then we still say it has a failure rate of 0% for
        convenience when applying thresholds.
        """

        if self.num_pass == self.num_fail == 0:
            return 0.0
        return self.num_fail / (self.num_pass + self.num_fail)

    def period_start_date(self, start_date, period_size):
        """Return a datetime.date() instance for the period start date.

        The result corresponds to the beginning of the time period containing 'self.start_date'.
        """

        if not isinstance(start_date, datetime.date):
            raise TypeError("'start_date' argument must be a date")

        if not isinstance(period_size, datetime.timedelta):
            raise TypeError("'period_size' argument must a datetime.timedelta instance")
        elif period_size.days <= 0:
            raise ValueError("'period_size' argument must be a positive number of days")
        elif period_size - datetime.timedelta(days=period_size.days) > datetime.timedelta():
            raise ValueError("'period_size' argument must be an integral number of days")

        # 'start_day_offset' is the number of days 'self.start_date' is from the start of the time
        # period.
        start_day_offset = (self.start_date - start_date).days % period_size.days
        return self.start_date - datetime.timedelta(days=start_day_offset)

    def week_start_date(self, start_day_of_week):
        """Return a datetime.date() instance of the week's start date.

        The result corresponds to the beginning of the week containing 'self.start_date'.
        The first day of the week can be specified as the strings "Sunday" or
        "Monday", as well as an arbitrary datetime.date() instance.
        """

        if isinstance(start_day_of_week, _STRING_TYPES):
            start_day_of_week = start_day_of_week.lower()
            if start_day_of_week == "sunday":
                start_weekday = 6
            elif start_day_of_week == "monday":
                start_weekday = 0
            else:
                raise ValueError(
                    "'start_day_of_week' can only be the string \"sunday\" or \"monday\"")
        elif isinstance(start_day_of_week, datetime.date):
            start_weekday = start_day_of_week.weekday()
        else:
            raise TypeError("'start_day_of_week' argument must be a string or a date")

        # 'start_day_offset' is the number of days 'self.start_date' is from the start of the week.
        start_day_offset = (self.start_date.weekday() - start_weekday) % 7
        return self.start_date - datetime.timedelta(days=start_day_offset)

    @classmethod
    def sum(cls, entries):
        """Return a single ReportEntry() instance.

        The result corresponds to all test executions represented by 'entries'.
        """

        test = set()
        task = set()
        variant = set()
        distro = set()
        start_date = cls._MAX_DATE
        end_date = cls._MIN_DATE
        num_pass = 0
        num_fail = 0

        for entry in entries:
            test.add(entry.test)
            task.add(entry.task)
            variant.add(entry.variant)
            distro.add(entry.distro)
            start_date = min(entry.start_date, start_date)
            end_date = max(entry.end_date, end_date)
            num_pass += entry.num_pass
            num_fail += entry.num_fail

        test = next(iter(test)) if len(test) == 1 else ReportEntry._MULTIPLE_TESTS
        task = next(iter(task)) if len(task) == 1 else ReportEntry._MULTIPLE_TASKS
        variant = next(iter(variant)) if len(variant) == 1 else ReportEntry._MULTIPLE_VARIANTS
        distro = next(iter(distro)) if len(distro) == 1 else ReportEntry._MULTIPLE_DISTROS

        return ReportEntry(test=test, task=task, variant=variant, distro=distro,
                           start_date=start_date, end_date=end_date, num_pass=num_pass,
                           num_fail=num_fail)


class Report(object):
    """Class for generating summarizations about Evergreen test executions."""

    TEST = ("test", )
    TEST_TASK = ("test", "task")
    TEST_TASK_VARIANT = ("test", "task", "variant")
    TEST_TASK_VARIANT_DISTRO = ("test", "task", "variant", "distro")

    DAILY = "daily"
    WEEKLY = "weekly"

    SUNDAY = "sunday"
    MONDAY = "monday"
    FIRST_DAY = "first-day"

    def __init__(self, entries):
        """Initialize the Report instance."""

        if not isinstance(entries, list):
            # It is possible that 'entries' is a generator function, so we convert it to a list in
            # order to be able to iterate it multiple times.
            entries = list(entries)

        if not entries:
            raise ValueError("Report cannot be created with no entries")

        self.start_date = min(entry.start_date for entry in entries)
        self.end_date = max(entry.end_date for entry in entries)

        self._entries = entries

    @property
    def raw_data(self):
        """Get a copy of the list of ReportEntry instances underlying the report."""

        return self._entries[:]

    def summarize_by(  # pylint: disable=too-many-branches,too-many-locals
            self, components, time_period=None, start_day_of_week=FIRST_DAY):
        """Return a list of ReportEntry instances grouped by the following.

        Grouping:
            'components' if 'time_period' is None,

            'components' followed by Entry.start_date if 'time_period' is "daily",

            'components' followed by Entry.week_start_date(start_day_of_week) if 'time_period' is
            "weekly". See Entry.week_start_date() for more details on the possible values for
            'start_day_of_week'.

            'components' followed by Entry.period_start_date(self.start_date, time_period) if
            'time_period' is a datetime.timedelta instance.
        """

        if not isinstance(components, (list, tuple)):
            raise TypeError("'components' argument must be a list or tuple")

        for component in components:
            if not isinstance(component, _STRING_TYPES):
                raise TypeError("Each element of 'components' argument must be a string")
            elif component not in ReportEntry._fields:
                raise ValueError("Each element of 'components' argument must be one of {}".format(
                    ReportEntry._fields))

        group_by = [operator.attrgetter(component) for component in components]

        if start_day_of_week == self.FIRST_DAY:
            start_day_of_week = self.start_date

        period_size = None
        if isinstance(time_period, _STRING_TYPES):
            if time_period == self.DAILY:
                group_by.append(operator.attrgetter("start_date"))
                period_size = datetime.timedelta(days=1)
            elif time_period == self.WEEKLY:
                group_by.append(lambda entry: entry.week_start_date(start_day_of_week))
                period_size = datetime.timedelta(days=7)
            else:
                raise ValueError(
                    "'time_period' argument can only be the string \"{}\" or \"{}\"".format(
                        self.DAILY, self.WEEKLY))
        elif isinstance(time_period, datetime.timedelta):
            group_by.append(lambda entry: entry.period_start_date(self.start_date, time_period))
            period_size = time_period
        elif time_period is not None:
            raise TypeError(("'time_period' argument must be a string or a datetime.timedelta"
                             " instance"))

        def key_func(entry):
            """Assign a key for sorting and grouping ReportEntry instances.

            The result is based on the combination of options summarize_by() was called with.
            """

            return [func(entry) for func in group_by]

        sorted_entries = sorted(self._entries, key=key_func)
        grouped_entries = itertools.groupby(sorted_entries, key=key_func)
        summed_entries = [ReportEntry.sum(group) for (_key, group) in grouped_entries]

        if period_size is not None and period_size.days > 1:
            # Overwrite the 'start_date' and 'end_date' attributes so that they correspond to the
            # beginning and end of the period, respectively. If the beginning or end of the week
            # falls outside the range [self.start_date, self.end_date], then the new 'start_date'
            # and 'end_date' attributes are clamped to that range.
            for (i, summed_entry) in enumerate(summed_entries):
                if time_period == self.WEEKLY:
                    period_start_date = summed_entry.week_start_date(start_day_of_week)
                else:
                    period_start_date = summed_entry.period_start_date(self.start_date, period_size)

                period_end_date = period_start_date + period_size - datetime.timedelta(days=1)
                start_date = max(period_start_date, self.start_date)
                end_date = min(period_end_date, self.end_date)
                summed_entries[i] = summed_entry._replace(start_date=start_date, end_date=end_date)

        return summed_entries


class Missing(object):
    """Class for representing the value associated with a particular component is unknown."""

    def __init__(self, kind):
        """Initialize Missing."""
        self._kind = kind

    def __eq__(self, other):
        if not isinstance(other, Missing):
            return NotImplemented

        return self._kind == other._kind  # pylint: disable=protected-access

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash(self._kind)

    def __str__(self):
        return "<unknown {}>".format(self._kind)


class TestHistory(object):
    """Class for interacting with the /test_stats Evergreen API endpoint."""

    DEFAULT_PROJECT = "mongodb-mongo-master"

    def __init__(  # pylint: disable=too-many-arguments
            self, project=DEFAULT_PROJECT, tests=None, tasks=None, variants=None, distros=None,
            retries=evergreen.EvergreenApiV2.DEFAULT_RETRIES):
        """Initialize the TestHistory instance with the list of tests, tasks, variants, and distros.

        The list of tests specified are augmented to ensure that failures on both POSIX and Windows
        platforms are returned by the Evergreen API.
        """

        tests = tests if tests is not None else []
        tests = [test for test_file in tests for test in self._denormalize_test_file(test_file)]

        self._tests = tests
        self._tasks = tasks if tasks is not None else []
        self._variants = variants if variants is not None else []
        self._distros = distros if distros is not None else []
        self._project = project

        self.evg_api = evergreen.get_evergreen_apiv2(num_retries=retries)

    def get_history_by_date(self, start_date, end_date, group_num_days=1):
        """Return a list of ReportEntry instances.

        The result corresponds to aggregated test executions between 'start_date' and
        'end_date'.
        """
        test_stats = self.evg_api.test_stats(
            self._project, start_date, end_date, group_num_days=group_num_days, tests=self._tests,
            tasks=self._tasks, variants=self._variants, distros=self._distros)

        history_data = []
        for test_stat in test_stats:
            history_data.append(self._process_test_result(test_stat, group_num_days))

        return history_data

    def _process_test_result(self, test_result, group_num_days):
        """Return a ReportEntry() tuple representing the 'test_result' dictionary."""

        # The end_date is computed as an offset from 'date'.
        end_date = _parse_date(test_result["date"]) + datetime.timedelta(days=group_num_days - 1)
        return ReportEntry(
            test=self._normalize_test_file(test_result["test_file"]), task=test_result.get(
                "task_name", Wildcard("tasks")), variant=test_result.get(
                    "variant", Wildcard("variants")), distro=test_result.get(
                        "distro", Wildcard("distros")), start_date=_parse_date(test_result["date"]),
            end_date=end_date, num_pass=test_result["num_pass"], num_fail=test_result["num_fail"])

    @staticmethod
    def _normalize_test_file(test_file):
        """Return normalized test_file name.

        If 'test_file' represents a Windows-style path, then it is converted to a POSIX-style path
        with

            - backslashes (\\) as the path separator replaced with forward slashes (/) and
            - the ".exe" extension, if present, removed.

        If 'test_file' already represents a POSIX-style path, then it is returned unmodified.
        """

        if "\\" in test_file:
            posix_test_file = test_file.replace("\\", "/")
            (test_file_root, test_file_ext) = os.path.splitext(posix_test_file)
            if test_file_ext == ".exe":
                return test_file_root
            return posix_test_file

        return test_file

    def _denormalize_test_file(self, test_file):
        """Return a list containing 'test_file' as both a POSIX-style and a Windows-style path.

        The conversion process may involving replacing forward slashes (/) as the path separator
        with backslashes (\\), as well as adding a ".exe" extension if 'test_file' has no file
        extension.
        """

        test_file = self._normalize_test_file(test_file)

        if "/" in test_file:
            windows_test_file = test_file.replace("/", "\\")
            if not os.path.splitext(test_file)[1]:
                windows_test_file += ".exe"
            return [test_file, windows_test_file]

        return [test_file]


def _parse_date(date_str):
    """Return a datetime.date instance representing the specified yyyy-mm-dd date string.

    Note that any time component of 'date_str', including the timezone, is ignored.
    """
    # We do not use strptime() because it is not thread safe (https://bugs.python.org/issue7980).
    year, month, day = date_str.split("T")[0].split("-")
    return datetime.date(int(year), int(month), int(day))


def _group_days(group_period):
    """Return a number of days for a group period."""
    if group_period == Report.DAILY:
        return 1
    elif group_period == Report.WEEKLY:
        return 7
    return int(group_period)


def main():  # pylint: disable=too-many-locals
    """Execute computing test failure rates from the Evergreen API."""

    parser = optparse.OptionParser(description=main.__doc__,
                                   usage="Usage: %prog [options] [test1 test2 ...]")

    parser.add_option("--project", dest="project", metavar="<project-name>",
                      default=TestHistory.DEFAULT_PROJECT,
                      help="The Evergreen project to analyze. Defaults to '%default'.")

    today = datetime.datetime.utcnow().replace(microsecond=0, tzinfo=None)
    parser.add_option("--sinceDate", dest="since_date", metavar="<yyyy-mm-dd>",
                      default="{:%Y-%m-%d}".format(today - datetime.timedelta(days=6)),
                      help=("The starting period as a date in UTC to analyze the test history for,"
                            " including the specified date. Defaults to 1 week ago (%default)."))

    parser.add_option("--untilDate", dest="until_date", metavar="<yyyy-mm-dd>",
                      default="{:%Y-%m-%d}".format(today),
                      help=("The ending period as a date in UTC to analyze the test history for,"
                            " including the specified date. Defaults to today (%default)."))

    parser.add_option("--groupPeriod", dest="group_period", metavar="[{}]".format(
        "|".join([Report.DAILY, Report.WEEKLY, "<ndays>"])), default=Report.WEEKLY,
                      help=("The time period over which to group test executions. Defaults to"
                            " '%default'."))

    parser.add_option("--weekStartDay", dest="start_day_of_week",
                      choices=(Report.SUNDAY, Report.MONDAY,
                               Report.FIRST_DAY), metavar="[{}]".format(
                                   "|".join([Report.SUNDAY, Report.MONDAY,
                                             Report.FIRST_DAY])), default=Report.FIRST_DAY,
                      help=("The day to use as the beginning of the week when grouping over time."
                            " This option is only relevant in conjuction with --groupPeriod={}. If"
                            " '{}' is specified, then the day of week of the earliest date is used"
                            " as the beginning of the week. Defaults to '%default'.".format(
                                Report.WEEKLY, Report.FIRST_DAY)))

    parser.add_option("--tasks", dest="tasks", metavar="<task1,task2,...>", default=None,
                      help="Comma-separated list of Evergreen task names to analyze.")

    parser.add_option("--variants", dest="variants", metavar="<variant1,variant2,...>",
                      default=None,
                      help="Comma-separated list of Evergreen build variants to analyze.")

    parser.add_option("--distros", dest="distros", metavar="<distro1,distro2,...>", default=None,
                      help="Comma-separated list of Evergreen build distros to analyze.")

    parser.add_option("--numRequestRetries", dest="num_request_retries",
                      metavar="<num-request-retries>",
                      default=evergreen.EvergreenApiV2.DEFAULT_RETRIES,
                      help=("The number of times a request to the Evergreen API will be retried on"
                            " failure. Defaults to '%default'."))

    log_levels = ["critical", "debug", "error", "info", "notset", "warning"]
    parser.add_option("--logLevel", dest="log_level", default="error", choices=log_levels,
                      help="Set the log level, from {}.".format(log_levels))

    (options, tests) = parser.parse_args()

    for (option_name, option_dest) in (("--sinceDate", "since_date"), ("--untilDate",
                                                                       "until_date")):
        option_value = getattr(options, option_dest)
        try:
            setattr(options, option_dest, _parse_date(option_value))
        except ValueError:
            parser.print_help(file=sys.stderr)
            print(file=sys.stderr)
            parser.error("{} must be specified in yyyy-mm-dd format, but got {}".format(
                option_name, option_value))

    group_period = options.group_period
    if options.group_period not in (Report.DAILY, Report.WEEKLY):
        try:
            group_period = datetime.timedelta(days=int(options.group_period))
        except ValueError:
            parser.print_help(file=sys.stderr)
            print(file=sys.stderr)
            parser.error("--groupPeriod must be an integral number, but got {}".format(
                options.group_period))

    if not options.tasks and not tests:
        parser.print_help(file=sys.stderr)
        print(file=sys.stderr)
        parser.error("Must specify either --tasks or at least one test")

    logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s",
                        level=options.log_level.upper())
    logging.Formatter.converter = time.gmtime

    tasks = options.tasks.split(",") if options.tasks else None
    variants = options.variants.split(",") if options.variants else None
    distros = options.distros.split(",") if options.distros else None
    test_history = TestHistory(project=options.project, tests=tests, tasks=tasks, variants=variants,
                               distros=distros, retries=options.num_request_retries)

    history_data = test_history.get_history_by_date(
        start_date=options.since_date, end_date=options.until_date, group_num_days=_group_days(
            options.group_period))

    report = Report(history_data)
    summary = report.summarize_by(Report.TEST_TASK_VARIANT_DISTRO, time_period=group_period,
                                  start_day_of_week=options.start_day_of_week)

    for entry in summary:
        print("(test={e.test},"
              " task={e.task},"
              " variant={e.variant},"
              " distro={e.distro},"
              " start_date={e.start_date:%Y-%m-%d},"
              " end_date={e.end_date:%Y-%m-%d},"
              " num_pass={e.num_pass},"
              " num_fail={e.num_fail},"
              " fail_rate={e.fail_rate:0.2%})".format(e=entry))


if __name__ == "__main__":
    main()
