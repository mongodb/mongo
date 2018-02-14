#!/usr/bin/env python

"""
Utility for computing test failure rates from the Evergreen API.
"""

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
import warnings

try:
    from urlparse import urlparse
except ImportError:
    from urllib.parse import urlparse

import requests
import requests.exceptions
import yaml

LOGGER = logging.getLogger(__name__)

if sys.version_info[0] == 2:
    _STRING_TYPES = (basestring,)
else:
    _STRING_TYPES = (str,)


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
    """
    A class for representing there are multiple values associated with a particular component.
    """

    def __init__(self, kind):
        self._kind = kind

    def __eq__(self, other):
        if not isinstance(other, Wildcard):
            return NotImplemented

        return self._kind == other._kind

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash(self._kind)

    def __str__(self):
        return "<multiple {}>".format(self._kind)


class ReportEntry(_ReportEntry):
    """
    Holds information about Evergreen test executions.
    """

    _MULTIPLE_TESTS = Wildcard("tests")
    _MULTIPLE_TASKS = Wildcard("tasks")
    _MULTIPLE_VARIANTS = Wildcard("variants")
    _MULTIPLE_DISTROS = Wildcard("distros")

    _MIN_DATE = datetime.date(datetime.MINYEAR, 1, 1)
    _MAX_DATE = datetime.date(datetime.MAXYEAR, 12, 31)

    @property
    def fail_rate(self):
        """
        Returns the fraction of test failures to total number of test executions.

        If a test hasn't been run at all, then we still say it has a failure rate of 0% for
        convenience when applying thresholds.
        """

        if self.num_pass == self.num_fail == 0:
            return 0.0
        return self.num_fail / (self.num_pass + self.num_fail)

    def period_start_date(self, start_date, period_size):
        """
        Returns a datetime.date() instance corresponding to the beginning of the time period
        containing 'self.start_date'.
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
        """
        Returns a datetime.date() instance corresponding to the beginning of the week containing
        'self.start_date'. The first day of the week can be specified as the strings "Sunday" or
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
        """
        Returns a single ReportEntry() instance corresponding to all test executions represented by
        'entries'.
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

        return ReportEntry(test=test,
                           task=task,
                           variant=variant,
                           distro=distro,
                           start_date=start_date,
                           end_date=end_date,
                           num_pass=num_pass,
                           num_fail=num_fail)


class Report(object):
    """
    A class for generating summarizations about Evergreen test executions.
    """

    TEST = ("test",)
    TEST_TASK = ("test", "task")
    TEST_TASK_VARIANT = ("test", "task", "variant")
    TEST_TASK_VARIANT_DISTRO = ("test", "task", "variant", "distro")

    DAILY = "daily"
    WEEKLY = "weekly"

    SUNDAY = "sunday"
    MONDAY = "monday"
    FIRST_DAY = "first-day"

    def __init__(self, entries):
        """
        Initializes the Report instance.
        """

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
        """
        Returns a copy of the list of ReportEntry instances underlying the report.
        """

        return self._entries[:]

    def summarize_by(self, components, time_period=None, start_day_of_week=FIRST_DAY):
        """
        Returns a list of ReportEntry instances grouped by

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
                raise ValueError(
                    "Each element of 'components' argument must be one of {}".format(
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
            """
            Assigns a key for sorting and grouping ReportEntry instances based on the combination of
            options summarize_by() was called with.
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
    """
    A class for representing the value associated with a particular component is unknown.
    """

    def __init__(self, kind):
        self._kind = kind

    def __eq__(self, other):
        if not isinstance(other, Missing):
            return NotImplemented

        return self._kind == other._kind

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash(self._kind)

    def __str__(self):
        return "<unknown {}>".format(self._kind)


class TestHistory(object):
    """
    A class for interacting with the /test_history Evergreen API endpoint.
    """

    DEFAULT_API_SERVER = "https://evergreen.mongodb.com"
    DEFAULT_PROJECT = "mongodb-mongo-master"

    DEFAULT_TEST_STATUSES = ("pass", "fail", "silentfail")
    DEFAULT_TASK_STATUSES = ("success", "failed", "timeout", "sysfail")

    # The Evergreen API requires specifying the "limit" parameter when not specifying a range of
    # revisions.
    DEFAULT_LIMIT = 20

    DEFAULT_NUM_RETRIES = 5

    _MISSING_DISTRO = Missing("distro")

    def __init__(self,
                 api_server=DEFAULT_API_SERVER,
                 project=DEFAULT_PROJECT,
                 tests=None,
                 tasks=None,
                 variants=None,
                 distros=None):
        """
        Initializes the TestHistory instance with the list of tests, tasks, variants, and distros
        specified.

        The list of tests specified are augmented to ensure that failures on both POSIX and Windows
        platforms are returned by the Evergreen API.
        """

        tests = tests if tests is not None else []
        tests = [test for test_file in tests for test in self._denormalize_test_file(test_file)]

        self._tests = tests
        self._tasks = tasks if tasks is not None else []
        self._variants = variants if variants is not None else []
        self._distros = distros if distros is not None else []

        # The number of API call retries on error. It can be set to 0 to disable the feature.
        self.num_retries = TestHistory.DEFAULT_NUM_RETRIES

        self._test_history_url = "{api_server}/rest/v1/projects/{project}/test_history".format(
            api_server=api_server,
            project=project,
        )

    def get_history_by_revision(self,
                                start_revision,
                                end_revision,
                                test_statuses=DEFAULT_TEST_STATUSES,
                                task_statuses=DEFAULT_TASK_STATUSES):
        """
        Returns a list of ReportEntry instances corresponding to each individual test execution
        between 'start_revision' and 'end_revision'.

        Only tests with status 'test_statuses' are included in the result. Similarly, only tests
        with status 'task_statuses' are included in the result. By default, both passing and failing
        test executions are returned.
        """

        params = self._history_request_params(test_statuses, task_statuses)
        params["beforeRevision"] = end_revision

        history_data = []

        # Since the API limits the results, with each invocation being distinct, we can simulate
        # pagination by making subsequent requests using "afterRevision".
        while start_revision != end_revision:
            params["afterRevision"] = start_revision

            test_results = self._get_history(params)
            if not test_results:
                break

            for test_result in test_results:
                history_data.append(self._process_test_result(test_result))

            # The first test will have the latest revision for this result set because
            # TestHistory._history_request_params() sorts by "latest".
            start_revision = test_results[0]["revision"]

        return history_data

    def get_history_by_date(self,
                            start_date,
                            end_date,
                            test_statuses=DEFAULT_TEST_STATUSES,
                            task_statuses=DEFAULT_TASK_STATUSES):
        """
        Returns a list of ReportEntry instances corresponding to each individual test execution
        between 'start_date' and 'end_date'.

        Only tests with status 'test_statuses' are included in the result. Similarly, only tests
        with status 'task_statuses' are included in the result. By default, both passing and failing
        test executions are returned.
        """

        warnings.warn(
            "Until https://jira.mongodb.org/browse/EVG-1653 is implemented, pagination using dates"
            " isn't guaranteed to returned a complete result set. It is possible for the results"
            " from an Evergreen task that started between the supplied start date and the"
            " response's latest test start time to be omitted.", RuntimeWarning)

        params = self._history_request_params(test_statuses, task_statuses)
        params["beforeDate"] = "{:%Y-%m-%d}T23:59:59Z".format(end_date)
        params["limit"] = self.DEFAULT_LIMIT

        start_time = "{:%Y-%m-%d}T00:00:00Z".format(start_date)
        history_data = set()

        # Since the API limits the results, with each invocation being distinct, we can simulate
        # pagination by making subsequent requests using "afterDate" and being careful to filter out
        # duplicate test results.
        while True:
            params["afterDate"] = start_time

            test_results = self._get_history(params)
            if not test_results:
                break

            original_size = len(history_data)
            for test_result in test_results:
                start_time = max(test_result["start_time"], start_time)
                history_data.add(self._process_test_result(test_result))

            # To prevent an infinite loop, we need to bail out if test results returned by the
            # request were identical to the ones we got back in an earlier request.
            if original_size == len(history_data):
                break

        return list(history_data)

    def _get_history(self, params):
        """
        Calls the test_history API endpoint with the given parameters and returns the JSON result.

        The API calls will be retried on HTTP and connection errors.
        """
        retries = 0
        while True:
            try:
                LOGGER.debug("Request to the test_history endpoint")
                start = time.time()
                response = requests.get(url=self._test_history_url, params=params)
                LOGGER.debug("Request took %fs", round(time.time() - start, 2))
                response.raise_for_status()
                return self._get_json(response)
            except (requests.exceptions.HTTPError,
                    requests.exceptions.ConnectionError, JSONResponseError) as err:
                if isinstance(err, JSONResponseError):
                    err = err.cause
                retries += 1
                LOGGER.error("Error while querying the test_history API: %s", str(err))
                if retries > self.num_retries:
                    raise err
                # We use 'retries' as the value for the backoff duration to space out the
                # requests when doing multiple retries.
                backoff_secs = retries
                LOGGER.info("Retrying after %ds", backoff_secs)
                time.sleep(backoff_secs)
            except:
                LOGGER.exception("Unexpected error while querying the test_history API"
                                 " with params: %s", params)
                raise

    @staticmethod
    def _get_json(response):
        try:
            return response.json()
        except ValueError as err:
            # ValueError can be raised if the connection is interrupted and we receive a truncated
            # json response. We raise a JSONResponseError instead to distinguish this error from
            # other ValueErrors.
            raise JSONResponseError(err)

    def _process_test_result(self, test_result):
        """
        Returns a ReportEntry() tuple representing the 'test_result' dictionary.
        """

        # For individual test executions, we intentionally use the "start_time" of the test as both
        # its 'start_date' and 'end_date' to avoid complicating how the test history is potentially
        # summarized by time. By the time the test has started, the Evergreen task has already been
        # assigned to a particular machine and is using a specific set of binaries, so there's
        # unlikely to be a significance to when the test actually finishes.
        start_date = end_date = _parse_date(test_result["start_time"])

        return ReportEntry(
            test=self._normalize_test_file(test_result["test_file"]),
            task=test_result["task_name"],
            variant=test_result["variant"],
            distro=test_result.get("distro", self._MISSING_DISTRO),
            start_date=start_date,
            end_date=end_date,
            num_pass=(1 if test_result["test_status"] == "pass" else 0),
            num_fail=(1 if test_result["test_status"] not in ("pass", "skip") else 0))

    @staticmethod
    def _normalize_test_file(test_file):
        """
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
        """
        Returns a list containing 'test_file' as both a POSIX-style path and a Windows-style path.

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

    def _history_request_params(self, test_statuses, task_statuses):
        """
        Returns the query parameters for /test_history GET request as a dictionary.
        """

        return {
            "distros": ",".join(self._distros),
            "sort": "latest",
            "tasks": ",".join(self._tasks),
            "tests": ",".join(self._tests),
            "taskStatuses": ",".join(task_statuses),
            "testStatuses": ",".join(test_statuses),
            "variants": ",".join(self._variants),
        }


def _parse_date(date_str):
    """
    Returns a datetime.date instance representing the specified yyyy-mm-dd date string.

    Note that any time component of 'date_str', including the timezone, is ignored.
    """
    # We do not use strptime() because it is not thread safe (https://bugs.python.org/issue7980).
    year, month, day = date_str.split("T")[0].split("-")
    return datetime.date(int(year), int(month), int(day))


class JSONResponseError(Exception):
    """An exception raised when failing to decode the JSON from an Evergreen response."""

    def __init__(self, cause):
        """Initializes the JSONResponseError with the exception raised by the requests library
        when decoding the response."""
        self.cause = cause


def main():
    """
    Utility computing test failure rates from the Evergreen API.
    """

    parser = optparse.OptionParser(description=main.__doc__,
                                   usage="Usage: %prog [options] [test1 test2 ...]")

    parser.add_option("--project", dest="project",
                      metavar="<project-name>",
                      default=TestHistory.DEFAULT_PROJECT,
                      help="The Evergreen project to analyze. Defaults to '%default'.")

    today = datetime.datetime.utcnow().replace(microsecond=0, tzinfo=None)
    parser.add_option("--sinceDate", dest="since_date",
                      metavar="<yyyy-mm-dd>",
                      default="{:%Y-%m-%d}".format(today - datetime.timedelta(days=6)),
                      help=("The starting period as a date in UTC to analyze the test history for,"
                            " including the specified date. Defaults to 1 week ago (%default)."))

    parser.add_option("--untilDate", dest="until_date",
                      metavar="<yyyy-mm-dd>",
                      default="{:%Y-%m-%d}".format(today),
                      help=("The ending period as a date in UTC to analyze the test history for,"
                            " including the specified date. Defaults to today (%default)."))

    parser.add_option("--sinceRevision", dest="since_revision",
                      metavar="<gitrevision>",
                      default=None,
                      help=("The starting period as a git revision to analyze the test history for,"
                            " excluding the specified commit. This option must be specified in"
                            " conjuction with --untilRevision and takes precedence over --sinceDate"
                            " and --untilDate."))

    parser.add_option("--untilRevision", dest="until_revision",
                      metavar="<gitrevision>",
                      default=None,
                      help=("The ending period as a git revision to analyze the test history for,"
                            " including the specified commit. This option must be specified in"
                            " conjuction with --sinceRevision and takes precedence over --sinceDate"
                            " and --untilDate."))

    parser.add_option("--groupPeriod", dest="group_period",
                      metavar="[{}]".format("|".join([Report.DAILY, Report.WEEKLY, "<ndays>"])),
                      default=Report.WEEKLY,
                      help=("The time period over which to group test executions. Defaults to"
                            " '%default'."))

    parser.add_option("--weekStartDay", dest="start_day_of_week",
                      choices=(Report.SUNDAY, Report.MONDAY, Report.FIRST_DAY),
                      metavar="[{}]".format(
                          "|".join([Report.SUNDAY, Report.MONDAY, Report.FIRST_DAY])),
                      default=Report.FIRST_DAY,
                      help=("The day to use as the beginning of the week when grouping over time."
                            " This option is only relevant in conjuction with --groupPeriod={}. If"
                            " '{}' is specified, then the day of week of the earliest date is used"
                            " as the beginning of the week. Defaults to '%default'.".format(
                                Report.WEEKLY, Report.FIRST_DAY)))

    parser.add_option("--tasks", dest="tasks",
                      metavar="<task1,task2,...>",
                      default="",
                      help="Comma-separated list of Evergreen task names to analyze.")

    parser.add_option("--variants", dest="variants",
                      metavar="<variant1,variant2,...>",
                      default="",
                      help="Comma-separated list of Evergreen build variants to analyze.")

    parser.add_option("--distros", dest="distros",
                      metavar="<distro1,distro2,...>",
                      default="",
                      help="Comma-separated list of Evergreen build distros to analyze.")

    parser.add_option("--numRequestRetries", dest="num_request_retries",
                      metavar="<num-request-retries>",
                      default=TestHistory.DEFAULT_NUM_RETRIES,
                      help=("The number of times a request to the Evergreen API will be retried on"
                            " failure. Defaults to '%default'."))

    (options, tests) = parser.parse_args()

    for (option_name, option_dest) in (("--sinceDate", "since_date"),
                                       ("--untilDate", "until_date")):
        option_value = getattr(options, option_dest)
        try:
            setattr(options,
                    option_dest,
                    _parse_date(option_value))
        except ValueError:
            parser.print_help(file=sys.stderr)
            print(file=sys.stderr)
            parser.error("{} must be specified in yyyy-mm-dd format, but got {}".format(
                option_name, option_value))

    if options.since_revision and not options.until_revision:
        parser.print_help(file=sys.stderr)
        print(file=sys.stderr)
        parser.error("Must specify --untilRevision in conjuction with --sinceRevision")
    elif options.until_revision and not options.since_revision:
        parser.print_help(file=sys.stderr)
        print(file=sys.stderr)
        parser.error("Must specify --sinceRevision in conjuction with --untilRevision")

    if options.group_period not in (Report.DAILY, Report.WEEKLY):
        try:
            options.group_period = datetime.timedelta(days=int(options.group_period))
        except ValueError:
            parser.print_help(file=sys.stderr)
            print(file=sys.stderr)
            parser.error("--groupPeriod must be an integral number, but got {}".format(
                options.group_period))

    if not options.tasks and not tests:
        parser.print_help(file=sys.stderr)
        print(file=sys.stderr)
        parser.error("Must specify either --tasks or at least one test")

    def read_evg_config():
        """
        Attempts to parse the user's or system's Evergreen configuration from its known locations.

        Returns None if the configuration file wasn't found anywhere.
        """

        known_locations = [
            "./.evergreen.yml",
            os.path.expanduser("~/.evergreen.yml"),
            os.path.expanduser("~/cli_bin/.evergreen.yml"),
        ]

        for filename in known_locations:
            if os.path.isfile(filename):
                with open(filename, "r") as fstream:
                    return yaml.safe_load(fstream)

        return None

    evg_config = read_evg_config()
    evg_config = evg_config if evg_config is not None else {}
    api_server = "{url.scheme}://{url.netloc}".format(
        url=urlparse(evg_config.get("api_server_host", TestHistory.DEFAULT_API_SERVER)))

    test_history = TestHistory(api_server=api_server,
                               project=options.project,
                               tests=tests,
                               tasks=options.tasks.split(","),
                               variants=options.variants.split(","),
                               distros=options.distros.split(","))
    test_history.num_retries = options.num_request_retries

    if options.since_revision:
        history_data = test_history.get_history_by_revision(
            start_revision=options.since_revision,
            end_revision=options.until_revision)
    elif options.since_date:
        history_data = test_history.get_history_by_date(
            start_date=options.since_date,
            end_date=options.until_date)

    report = Report(history_data)
    summary = report.summarize_by(Report.TEST_TASK_VARIANT_DISTRO,
                                  time_period=options.group_period,
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
