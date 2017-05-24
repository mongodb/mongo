#!/usr/bin/env python

"""Test Failures

Compute Test failures rates from Evergreen API for specified tests, tasks, etc.
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import datetime
import itertools
import operator
from optparse import OptionParser
import os
import urlparse

import requests
import yaml

_API_SERVER_DEFAULT = "http://evergreen-api.mongodb.com:8080"
_REST_PREFIX = "/rest/v1"
_PROJECT = "mongodb-mongo-master"
_MIN_DATE = "0001-01-01"
_MAX_DATE = "3000-12-31"

_HistoryReportTuple = collections.namedtuple(
    "Report", "test task variant distro start_dt test_status")


def read_evg_config():
    # Expand out evergreen config file possibilities
    file_list = [
        "./.evergreen.yml",
        os.path.expanduser("~/.evergreen.yml"),
        os.path.expanduser("~/cli_bin/.evergreen.yml")]

    for filename in file_list:
        if os.path.isfile(filename):
            with open(filename, "r") as fstream:
                return yaml.load(fstream)
    return None


def datestr_to_date(date_str):
    """Returns datetime from a date string in the format of YYYY-MM-DD.
       Note that any time in the date string is stripped off."""
    return datetime.datetime.strptime(date_str.split("T")[0], "%Y-%m-%d").date()


def date_to_datestr(date_time):
    """Returns date string in the format of YYYY-MM-DD from a datetime."""
    return date_time.strftime("%Y-%m-%d")


def list_or_none(lst):
    """Returns a stringified list or 'None'."""
    return ",".join(map(str, lst)) if lst else "None"


def normalize_test_file(test_file):
    """Normalizes the test_file name:
       - Changes single backslash (\\) to forward slash (/)
       - Removes .exe extension
       Returns normalized string."""
    return test_file.replace("\\", "/").replace(".exe", "")


def fail_rate(num_fail, num_pass):
    """Computes fails rate, return N/A if total is 0."""
    total = num_fail + num_pass
    if total:
        return "{:.3f}".format(round(num_fail / total, 3))
    return "N/A"


class Missing(object):
    """Class to support missing fields from the history report."""
    def __init__(self, kind):
        self.kind = kind

    def __str__(self):
        return self.kind


class ViewReport(object):
    """"Class to support any views into the HistoryReport."""

    Summary = collections.namedtuple(
        "Summary",
        "test task variant distro start_date end_date fail_rate num_fail num_pass")

    DetailGroup = collections.namedtuple(
        "DetailGroup",
        "test task variant distro start_date end_date")

    group_by = ["test", "task", "variant", "distro"]
    group_period_values = ["daily", "weekly"]
    start_days = ["first_day", "sunday", "monday"]

    def __init__(self, history_report, group_period="weekly", start_day_of_week="first_day"):
        self._report = history_report
        self.group_period = group_period.lower()
        if self.group_period not in self.group_period_values:
            raise ValueError(
                "Invalid group_period specified '{}'".format(self.group_period))
        self.group_days = self._num_days_for_group()
        self.start_day_of_week = start_day_of_week.lower()
        # Using 'first_day' means the a weekly group report will start on the day of the
        # week from the earliest date in the test history.
        if self.start_day_of_week not in self.start_days:
            raise ValueError(
                "Invalid start_day_of_week specified '{}'".format(self.start_day_of_week))
        if history_report:
            start_dts = [r.start_dt for r in history_report]
            self.start_dt = min(start_dts)
            self.end_dt = max(start_dts)
        else:
            self.start_dt = datestr_to_date(_MAX_DATE)
            self.end_dt = datestr_to_date(_MIN_DATE)

    def _num_days_for_group(self):
        """Returns the number of days defined in the self.group_period."""
        if self.group_period == "daily":
            return 1
        return 7

    def _group_dates(self, test_dt):
        """Returns start_date and end_date for the group_period, which are are included
           in the group_period."""
        # Computing the start and end dates for a weekly period may have special cases for the
        # first and last periods. Since the first period may not start on the weekday for
        # self.start_day_of_week (if it's 'sunday' or 'monday'), that period may be less than 7
        # days. Similarly the last period will always end on self.end_dt.
        # Example, if the start_date falls on a Wednesday, then all group starting
        # dates are offset from that, if self.start_day_of_week is 'first_day'.

        # The start date for a 'weekly' group_period is one of the following:
        # - self.start_dt (the earliest date in the report)
        # - The day specified in self.start_day_of_week
        # - A weekly offset from self.start_dt, if self.start_day_of_week is 'first_day'
        # The ending date for a 'weekly' group_period is one of the following:
        # - self.end_dt (the latest date in the report)
        # - The mod of difference of weekday of test_dt and the start_weekday

        if test_dt < self.start_dt or test_dt > self.end_dt:
            raise ValueError("The test_dt {} must be >= {} and <= {}".format(
                test_dt, self.start_dt, self.end_dt))

        if self.group_period == "daily":
            return (test_dt, test_dt)

        if self.start_day_of_week == "sunday":
            start_weekday = 6
        elif self.start_day_of_week == "monday":
            start_weekday = 0
        elif self.start_day_of_week == "first_day":
            start_weekday = self.start_dt.weekday()
        # 'start_day_offset' is the number of days 'test_dt' is from the start of the week.
        start_day_offset = (test_dt.weekday() - start_weekday) % 7
        group_start_dt = test_dt - datetime.timedelta(days=start_day_offset)
        group_end_dt = group_start_dt + datetime.timedelta(days=6)
        return (max(group_start_dt, self.start_dt), min(group_end_dt, self.end_dt))

    def _select_attribute(self, value, attributes):
        """Returns true if attribute value list is None or a value matches from the list of
           attribute values."""
        return not attributes or value in attributes

    def _filter_reports(self,
                        start_date=_MIN_DATE,
                        end_date=_MAX_DATE,
                        tests=None,
                        tasks=None,
                        variants=None,
                        distros=None):
        """Returns filter of self._report."""
        return [r for r in self._report
                if r.start_dt >= datestr_to_date(start_date) and
                r.start_dt <= datestr_to_date(end_date) and
                self._select_attribute(r.test, tests) and
                self._select_attribute(r.task, tasks) and
                self._select_attribute(r.variant, variants) and
                (r.distro is None or self._select_attribute(r.distro, distros))]

    def _detail_report(self, report):
        """Returns the detailed report, which is a dictionary in the form of key tuples,
           '(test, task, variant, distro, start_date, end_date)', with a value of
           {num_pass, num_fail}."""
        detail_report = {}
        for record in report:
            group_start_dt, group_end_dt = self._group_dates(record.start_dt)
            detail_group = self.DetailGroup(
                test=record.test,
                task=record.task,
                variant=record.variant,
                distro=record.distro,
                start_date=group_start_dt,
                end_date=group_end_dt)
            detail_report.setdefault(detail_group, {"num_pass": 0, "num_fail": 0})
            if record.test_status == "pass":
                status_key = "num_pass"
            else:
                status_key = "num_fail"
            detail_report[detail_group][status_key] += 1
        return detail_report

    def _summary_report(self, report, tests=None, tasks=None, variants=None, distros=None):
        """Returns the summary report for the specifed combinations of paramters. The format
           is a nametuple, with {num_pass, num_fail} based on the _detailed_report."""
        summary_report = {}
        if not report:
            return summary_report
        start_dt = min([r.start_dt for r in report])
        end_dt = max([r.start_dt for r in report])
        num_pass = sum([r.test_status == "pass" for r in report])
        num_fail = sum([r.test_status != "pass" for r in report])
        detail_group = self.DetailGroup(
            test=list_or_none(tests),
            task=list_or_none(tasks),
            variant=list_or_none(variants),
            distro=list_or_none(distros),
            start_date=start_dt,
            end_date=end_dt)
        summary_report[detail_group] = {"num_pass": num_pass, "num_fail": num_fail}
        return summary_report

    def view_detail(self, tests=None, tasks=None, variants=None, distros=None):
        """Provides a detailed view of specified parameters.
           The parameters are used as a filter, so an unspecified parameter provides
           more results.
           Returns the view as a list of namedtuples:
                (test, task, variant, distro, start_date, end_date, fail_rate, num_fail, num_pass)
        """

        filter_results = self._filter_reports(
            tests=tests, tasks=tasks, variants=variants, distros=distros)

        view_report = []
        detail_report = self._detail_report(filter_results)
        for detail_group in detail_report:
            view_report.append(self.Summary(test=detail_group.test,
                                            task=detail_group.task,
                                            variant=detail_group.variant,
                                            distro=detail_group.distro,
                                            start_date=detail_group.start_date,
                                            end_date=detail_group.end_date,
                                            fail_rate=fail_rate(
                                                detail_report[detail_group]["num_fail"],
                                                detail_report[detail_group]["num_pass"]),
                                            num_fail=detail_report[detail_group]["num_fail"],
                                            num_pass=detail_report[detail_group]["num_pass"]))
        return sorted(view_report)

    def view_summary_groups(self, group_on=None):
        """Provides a summary view report, based on the group_on list, for each self.group_period.
           If group_on is empty, then a total summary report is provided.
           Returns the view as a sorted list of namedtuples:
                (test, task, variant, distro, start_date, end_date, fail_rate, num_fail, num_pass)
        """

        group_on = group_on if group_on is not None else []

        # Discover all group_period date ranges
        group_periods = set()
        dt = self.start_dt
        while dt <= self.end_dt:
            group_periods.add(self._group_dates(dt))
            dt += datetime.timedelta(days=1)

        view_report = []
        for (start_dt, end_dt) in group_periods:
            view_report.extend(self.view_summary(group_on,
                                                 start_date=date_to_datestr(start_dt),
                                                 end_date=date_to_datestr(end_dt)))
        return sorted(view_report)

    def view_summary(self, group_on=None, start_date=_MIN_DATE, end_date=_MAX_DATE):
        """Provides a summary view report, based on the group_on list. If group_on is empty, then
           a total summary report is provided.
           Returns the view as a sorted list of namedtuples:
                (test, task, variant, distro, start_date, end_date, fail_rate, num_fail, num_pass)
        """

        group_on = group_on if group_on is not None else []

        for group_name in group_on:
            if group_name not in self.group_by:
                raise ValueError("Invalid group '{}' specified, the supported groups are {}"
                                 .format(group_name, self.group_by))

        tests = list(set([r.test for r in self._report])) \
            if "test" in group_on else [Missing("__all_tests")]
        tasks = list(set([r.task for r in self._report])) \
            if "task" in group_on else [Missing("__all_tasks")]
        variants = list(set([r.variant for r in self._report])) \
            if "variant" in group_on else [Missing("__all_variants")]
        distros = list(set([str(r.distro) for r in self._report])) \
            if "distro" in group_on else [Missing("__all_distros")]

        group_lists = [tests, tasks, variants, distros]
        group_combos = list(itertools.product(*group_lists))
        view_report = []
        for group in group_combos:
            test_filter = [group[0]] if group[0] and not isinstance(group[0], Missing) else None
            task_filter = [group[1]] if group[1] and not isinstance(group[1], Missing) else None
            variant_filter = [group[2]] if group[2] and not isinstance(group[2], Missing) else None
            distro_filter = [group[3]] if group[3] and not isinstance(group[3], Missing) else None
            filter_results = self._filter_reports(start_date=start_date,
                                                  end_date=end_date,
                                                  tests=test_filter,
                                                  tasks=task_filter,
                                                  variants=variant_filter,
                                                  distros=distro_filter)
            summary_report = self._summary_report(filter_results,
                                                  tests=test_filter,
                                                  tasks=task_filter,
                                                  variants=variant_filter,
                                                  distros=distro_filter)
            for summary in summary_report:
                view_report.append(self.Summary(test=summary.test,
                                                task=summary.task,
                                                variant=summary.variant,
                                                distro=summary.distro,
                                                start_date=summary.start_date,
                                                end_date=summary.end_date,
                                                fail_rate=fail_rate(
                                                    summary_report[summary]["num_fail"],
                                                    summary_report[summary]["num_pass"]),
                                                num_fail=summary_report[summary]["num_fail"],
                                                num_pass=summary_report[summary]["num_pass"]))
        return sorted(view_report)


class HistoryReport(object):
    """The HistoryReport class interacts with the Evergreen REST API to generate a history_report.
       The history_report is meant to be viewed from the ViewReport class methods."""

    group_period_values = ["daily", "weekly"]

    # TODO EVG-1653: Uncomment this line once the --sinceDate and --untilDate options are exposed.
    # period_types = ["date", "revision"]
    period_types = ["revision"]

    def __init__(self,
                 period_type,
                 start,
                 end,
                 start_day_of_week="first_day",
                 group_period="weekly",
                 project=_PROJECT,
                 tests=None,
                 tasks=None,
                 variants=None,
                 distros=None,
                 evg_cfg=None):
        # Initialize the report and object variables.
        self._report_tuples = []
        self._start_date = _MAX_DATE
        self._end_date = _MIN_DATE
        self._report = {"tests": {}}
        self.period_type = period_type.lower()
        if self.period_type not in self.period_types:
            raise ValueError(
                "Invalid time period type '{}' specified."
                " supported types are {}.".format(self.period_type, self.period_types))
        self.group_period = group_period.lower()
        if self.group_period not in self.group_period_values:
            raise ValueError(
                "Invalid group_period '{}' specified,"
                " supported periods are {}.".format(self.group_period, self.group_period_values))
        self.start_day_of_week = start_day_of_week.lower()

        self.start = start
        self.end = end

        self.project = project

        if not tests and not tasks:
            raise ValueError("Must specify either tests or tasks.")
        self.tests = tests if tests is not None else []
        self.tasks = tasks if tasks is not None else []
        self.variants = variants if variants is not None else []
        self.distros = distros if distros is not None else []

        if evg_cfg is not None and "api_server_host" in evg_cfg:
            api_server = "{url.scheme}://{url.netloc}".format(
                url=urlparse.urlparse(evg_cfg["api_server_host"]))
        else:
            api_server = _API_SERVER_DEFAULT
        self.api_prefix = api_server + _REST_PREFIX

    def _all_tests(self):
        """Returns a list of all test file name types from self.tests.
           Since the test file names can be specifed as either Windows or Linux style,
           we will ensure that both are specified for each test.
           Add Windows style naming, backslashes and possibly .exe extension.
           Add Linux style naming, forward slashes and removes .exe extension."""
        tests_set = set(self.tests)
        for test in self.tests:
            if "/" in test:
                windows_test = test.replace("/", "\\")
                if not os.path.splitext(test)[1]:
                    windows_test += ".exe"
                tests_set.add(windows_test)
            if "\\" in test:
                linux_test = test.replace("\\", "/")
                linux_test = linux_test.replace(".exe", "")
                tests_set.add(linux_test)
        return list(tests_set)

    def _history_request_params(self, test_statuses):
        """Returns a dictionary of params used in requests.get."""
        return {
            "distros": ",".join(self.distros),
            "sort": "latest",
            "tasks": ",".join(self.tasks),
            "tests": ",".join(self.tests),
            "taskStatuses": "failed,timeout,success,sysfail",
            "testStatuses": ",".join(test_statuses),
            "variants": ",".join(self.variants),
            }

    def _get_history_by_revision(self, test_statuses):
        """ Returns a list of history data for specified options."""
        after_revision = self.start
        before_revision = self.end
        params = self._history_request_params(test_statuses)
        params["beforeRevision"] = before_revision
        url = "{prefix}/projects/{project}/test_history".format(
            prefix=self.api_prefix,
            project=self.project)

        # Since the API limits the results, with each invocation being distinct, we can
        # simulate pagination, by requesting results using afterRevision.
        history_data = []
        while after_revision != before_revision:
            params["afterRevision"] = after_revision
            response = requests.get(url=url, params=params)
            response.raise_for_status()
            if not response.json():
                break

            # The first test will have the latest revision for this result set.
            after_revision = response.json()[0]["revision"]
            history_data.extend(response.json())

        return history_data

    def _get_history_by_date(self, test_statuses):
        """ Returns a list of history data for specified options."""
        # Note this functionality requires EVG-1653
        start_date = self.start
        end_date = self.end
        params = self._history_request_params(test_statuses)
        params["beforeDate"] = end_date + "T23:59:59Z"
        url = "{prefix}/projects/{project}/test_history".format(
            prefix=self.api_prefix,
            project=self.project)

        # Since the API limits the results, with each invocation being distinct, we can
        # simulate pagination, by requesting results using afterDate, being careful to
        # filter out possible duplicate entries.
        start_time = start_date + "T00:00:00Z"
        history_data = []
        history_data_set = set()
        last_sorted_tests = []
        while True:
            params["afterDate"] = start_time
            response = requests.get(url=url, params=params)
            response.raise_for_status()
            if not response.json():
                return history_data

            sorted_tests = sorted(response.json(), key=operator.itemgetter("start_time"))

            # To prevent an infinite loop, we need to bail out if the result set is the same
            # as the previous one.
            if sorted_tests == last_sorted_tests:
                break

            last_sorted_tests = sorted_tests

            for test in sorted_tests:
                start_time = test["start_time"]
                # Create a unique hash for the test entry and check if it's been processed.
                test_hash = hash(str(sorted(test.items())))
                if test_hash not in history_data_set:
                    history_data_set.add(test_hash)
                    history_data.append(test)

        return history_data

    def generate_report(self):
        """Creates detail for self._report from specified test history options.
           Returns a ViewReport object of self._report."""

        if self.period_type == "date":
            report_method = self._get_history_by_date
        else:
            report_method = self._get_history_by_revision

        self.tests = self._all_tests()

        rest_api_report = report_method(test_statuses=["fail", "pass"])

        missing_distro = Missing("no_distro")
        for record in rest_api_report:
            self._start_date = min(self._start_date, record["start_time"])
            self._end_date = max(self._end_date, record["start_time"])
            # Save API record as namedtuple
            self._report_tuples.append(
                _HistoryReportTuple(
                    test=normalize_test_file(record["test_file"]),
                    task=record["task_name"],
                    variant=record["variant"],
                    distro=record.get("distro", missing_distro),
                    start_dt=datestr_to_date(record["start_time"]),
                    test_status=record["test_status"]))

        sorted_report = sorted(self._report_tuples, key=operator.attrgetter("start_dt"))

        return ViewReport(sorted_report,
                          group_period=self.group_period,
                          start_day_of_week=self.start_day_of_week)


def main():

    parser = OptionParser(description=__doc__, usage="Usage: %prog [options] test1 test2 ...")

    parser.add_option("--project", dest="project",
                      default=_PROJECT,
                      help="Evergreen project to analyze, defaults to '%default'.")

    # TODO EVG-1653: Expose the --sinceDate and --untilDate command line arguments after pagination
    # is made possible using the /test_history Evergreen API endpoint.
    # parser.add_option("--sinceDate", dest="start_date",
    #                   metavar="YYYY-MM-DD",
    #                   default="{:%Y-%m-%d}".format(today - datetime.timedelta(days=6)),
    #                   help="History from this date, defaults to 1 week ago (%default).")

    # parser.add_option("--untilDate", dest="end_date",
    #                   metavar="YYYY-MM-DD",
    #                   default="{:%Y-%m-%d}".format(today),
    #                   help="History up to, and including, this date, defaults to today (%default).")

    parser.add_option("--sinceRevision", dest="since_revision",
                      default=None,
                      help="History after this revision."
                           # TODO EVG-1653: Uncomment this line once the --sinceDate and --untilDate
                           # options are exposed.
                           # "History after this revision, overrides --sinceDate & --untilDate."
                           " Must be specified with --untilRevision")

    parser.add_option("--untilRevision", dest="until_revision",
                      default=None,
                      help="History up to, and including, this revision."
                           # TODO EVG-1653: Uncomment this line once the --sinceDate and
                           # --untilDate options are exposed.
                           # "History up to, and including, this revision, overrides"
                           # " --sinceDate & --untilDate."
                           " Must be specified with --sinceRevision")

    parser.add_option("--groupPeriod", dest="group_period",
                      choices=["daily", "weekly"],
                      default="weekly",
                      help="Set to 'daily' or 'weekly', defaults to '%default'.")

    parser.add_option("--weekStartDay", dest="start_day_of_week",
                      choices=["sunday", "monday", "first_day"],
                      default="first_day",
                      help="The group starting day of week, when --groupPeriod is set to'weekly'. "
                           " Set to 'sunday', 'monday' or 'first_day'."
                           " If 'first_day', the weekly group will start on the first day of the"
                           " starting date from the history result, defaults to '%default'.")

    parser.add_option("--tasks", dest="tasks",
                      default="",
                      help="Comma separated list of task display names to analyze.")

    parser.add_option("--variants", dest="variants",
                      default="",
                      help="Comma separated list of build variants to analyze.")

    parser.add_option("--distros", dest="distros",
                      default="",
                      help="Comma separated list of build distros to analyze.")

    (options, args) = parser.parse_args()

    # TODO EVG-1653: Uncomment these lines once the --sinceDate and --untilDate options are
    # exposed.
    # period_type = "date"
    # start = options.start_date
    # end = options.end_date

    if options.since_revision and options.until_revision:
        period_type = "revision"
        start = options.since_revision
        end = options.until_revision
    elif options.since_revision or options.until_revision:
        parser.print_help()
        parser.error("Must specify both --sinceRevision & --untilRevision")
    # TODO EVG-1653: Remove this else clause once the --sinceDate and --untilDate options are
    # exposed.
    else:
        parser.print_help()
        parser.error("Must specify both --sinceRevision & --untilRevision")

    if not options.tasks and not args:
        parser.print_help()
        parser.error("Must specify either --tasks or at least one test")

    report = HistoryReport(period_type=period_type,
                           start=start,
                           end=end,
                           group_period=options.group_period,
                           start_day_of_week=options.start_day_of_week,
                           project=options.project,
                           tests=args,
                           tasks=options.tasks.split(","),
                           variants=options.variants.split(","),
                           distros=options.distros.split(","),
                           evg_cfg=read_evg_config())
    view_report = report.generate_report()
    for record in view_report.view_detail():
        print(record)

if __name__ == "__main__":
    main()
