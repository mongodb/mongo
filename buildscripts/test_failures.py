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
import optparse
import os
import urlparse

import requests
import yaml

_API_SERVER_DEFAULT = "http://evergreen-api.mongodb.com:8080"
_REST_PREFIX = "/rest/v1"
_PROJECT = "mongodb-mongo-master"


class _Missing(object):
    """Class to support missing fields from the report."""
    def __init__(self, kind):
        self._kind = kind

    def __eq__(self, other):
        if not isinstance(other, _Missing):
            return NotImplemented
        return self._kind == other._kind

    def __ne__(self, other):
        return not self == other

    def __str__(self):
        return "<_Missing: {}>".format(self._kind)

_ALL_TEST = _Missing("test")
_ALL_TASK = _Missing("task")
_ALL_VARIANT = _Missing("variant")
_ALL_DISTRO = _Missing("distro")
_ALL_DATE = _Missing("date")


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


class ViewReport(object):
    """"Class to support any views into the HistoryReport."""

    DetailGroup = collections.namedtuple(
        "DetailGroup",
        "test task variant distro start_date end_date")

    Summary = collections.namedtuple(
        "Summary",
        "test task variant distro start_date end_date fail_rate num_fail num_pass")

    SummaryGroup = collections.namedtuple(
        "SummaryGroup",
        "test task variant distro start_date end_date")

    _MIN_DATE = "{0:04}-01-01".format(datetime.MINYEAR)
    _MAX_DATE = "{}-12-31".format(datetime.MAXYEAR)
    _group_by = ["test", "task", "variant", "distro"]
    _start_days = ["first_day", "sunday", "monday"]

    def __init__(self,
                 history_report=[],
                 group_period=7,
                 start_day_of_week="first_day"):
        self._report = history_report

        self.start_day_of_week = start_day_of_week
        # Using 'first_day' means the a group report will start on the day of the
        # week from the earliest date in the test history.
        if self.start_day_of_week not in self._start_days:
            raise ValueError(
                "Invalid start_day_of_week specified '{}'".format(self.start_day_of_week))

        # Set start and end dates of report and create the group_periods
        self.group_period = group_period
        if self._report:
            start_dts = [r.start_dt for r in self._report]
            self.start_dt = min(start_dts)
            self.end_dt = max(start_dts)
            self._group_periods = self._create_group_periods()
        else:
            self.start_dt = datestr_to_date(self._MIN_DATE)
            self.end_dt = datestr_to_date(self._MAX_DATE)
            self._group_periods = []

        self._summary_report = {}
        self._update_summary_report()

        # Create the lists of tests, tasks, variants & distros.
        self._all_tests = list(set([r.test for r in self._report]))
        self._all_tasks = list(set([r.task for r in self._report]))
        self._all_variants = list(set([r.variant for r in self._report]))
        self._all_distros = list(set([str(r.distro) for r in self._report]))

    def fail_rate(self, num_fail, num_pass):
        """Computes fails rate, return 0 if no tests have run."""
        if num_pass == num_fail == 0:
            return 0.0
        return num_fail / (num_pass + num_fail)

    def _group_dates(self, test_dt, from_end):
        """Returns start_date and end_date for the group_period, which are are included
           in the group_period."""
        # Computing the start and end dates for a period may have special cases for the
        # first and last periods, only if the self.group_period is 7, which represents weekly.
        # Since the first period may not start on the weekday for start_day_of_week
        # (if it's 'sunday' or 'monday'), that period may be less than the
        # period days. Similarly the last period will always end on end_dt.
        # Example, if the start_date falls on a Wednesday, then all group starting
        # dates are offset from that, if start_day_of_week is 'first_day'.

        # Use 'from_end=True' to produce group_dates for analyzing the report from the end.

        # The start date for a group_period is one of the following:
        # - start_dt (the earliest date in the report)
        # - The day specified in start_day_of_week
        # - An offset from start_dt, if start_day_of_week is 'first_day'
        # The ending date for a group_period is one of the following:
        # - end_dt (the latest date in the report)
        # - The mod of difference of weekday of test_dt and the start_weekday

        if test_dt < self.start_dt or test_dt > self.end_dt:
            raise ValueError("The test_dt {} must be >= {} and <= {}".format(
                test_dt, self.start_dt, self.end_dt))

        if self.group_period == 1:
            return (test_dt, test_dt)

        # Return group_dates relative to the end_dt. The start_day_of_week is not
        # used in computing the dates.
        if from_end:
            group_end_dt = min(
                self.end_dt,
                test_dt + datetime.timedelta(
                    days=((self.end_dt - test_dt).days % self.group_period)))
            group_st_dt = max(
                self.start_dt,
                group_end_dt - datetime.timedelta(days=self.group_period - 1))
            return (group_st_dt, group_end_dt)

        # When the self.group_period is 7, we support a start_day_of_week.
        if self.group_period == 7:
            if self.start_day_of_week == "sunday":
                start_weekday = 6
            elif self.start_day_of_week == "monday":
                start_weekday = 0
            elif self.start_day_of_week == "first_day":
                start_weekday = self.start_dt.weekday()
            # 'start_day_offset' is the number of days 'test_dt' is from the start of the week.
            start_day_offset = (test_dt.weekday() - start_weekday) % 7
        else:
            start_day_offset = (test_dt - self.start_dt).days % self.group_period

        group_start_dt = test_dt - datetime.timedelta(days=start_day_offset)
        group_end_dt = group_start_dt + datetime.timedelta(days=self.group_period - 1)
        return (max(group_start_dt, self.start_dt), min(group_end_dt, self.end_dt))

    def _select_attribute(self, value, attributes):
        """Returns true if attribute value list is None or a value matches from the list of
           attribute values."""
        return not attributes or value in attributes

    def _create_group_periods(self):
        """Discover all group_periods."""
        group_periods = set()
        test_dt = self.start_dt
        end_dt = self.end_dt
        while test_dt <= end_dt:
            # We will summarize for time periods from start-to-end and end-to-start.
            group_periods.add(self._group_dates(test_dt, False))
            group_periods.add(self._group_dates(test_dt, True))
            test_dt += datetime.timedelta(days=1)
        return group_periods

    def _update_summary_record(self, report_key, status_key):
        """Increments the self._summary_report report_key's status_key & fail_rate."""
        summary = self._summary_report.setdefault(
            report_key,
            {"num_fail": 0, "num_pass": 0, "fail_rate": 0.0})
        summary[status_key] += 1
        summary["fail_rate"] = self.fail_rate(summary["num_fail"], summary["num_pass"])

    def _update_summary_report(self):
        """Process self._report and updates the self._summary_report."""

        for record in self._report:
            if record.test_status == "pass":
                status_key = "num_pass"
            else:
                status_key = "num_fail"
            # Update each combination summary:
            #   _total_, test, test/task, test/task/variant, test/task/variant/distro
            for combo in ["_total_", "test", "task", "variant", "distro"]:
                test = record.test if combo != "_total_" else _ALL_TEST
                task = record.task if combo in ["task", "variant", "distro"] else _ALL_TASK
                variant = record.variant if combo in ["variant", "distro"] else _ALL_VARIANT
                distro = record.distro if combo == "distro" else _ALL_DISTRO
                # Update the summary for matching group periods.
                for (group_start_dt, group_end_dt) in self._group_periods:
                    if record.start_dt >= group_start_dt and record.start_dt <= group_end_dt:
                        report_key = self.SummaryGroup(
                            test=test,
                            task=task,
                            variant=variant,
                            distro=distro,
                            start_date=date_to_datestr(group_start_dt),
                            end_date=date_to_datestr(group_end_dt))
                        self._update_summary_record(report_key, status_key)
                # Update the summary for the entire date period.
                report_key = self.SummaryGroup(
                    test=test,
                    task=task,
                    variant=variant,
                    distro=distro,
                    start_date=_ALL_DATE,
                    end_date=_ALL_DATE)
                self._update_summary_record(report_key, status_key)

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
            group_start_dt, group_end_dt = self._group_dates(record.start_dt, False)
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

    def last_period(self):
        """Returns start_date and end_date for the last period in the report."""
        start_dt = max(self.start_dt,
                       self.end_dt - datetime.timedelta(days=self.group_period - 1))
        return date_to_datestr(start_dt), date_to_datestr(self.end_dt)

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
            view_report.append(self.Summary(
                test=detail_group.test,
                task=detail_group.task,
                variant=detail_group.variant,
                distro=detail_group.distro,
                start_date=detail_group.start_date,
                end_date=detail_group.end_date,
                fail_rate=self.fail_rate(
                    detail_report[detail_group]["num_fail"],
                    detail_report[detail_group]["num_pass"]),
                num_fail=detail_report[detail_group]["num_fail"],
                num_pass=detail_report[detail_group]["num_pass"]))
        return view_report

    def view_summary(self,
                     group_on=None,
                     start_date=_ALL_DATE,
                     end_date=_ALL_DATE):
        """Provides a summary view report, based on the group_on list. If group_on is empty, then
           a total summary report is provided. The start_date and end_date must match the
           group periods for a result to be returned.
           Returns the view as a list of namedtuples:
                (test, task, variant, distro, start_date, end_date, fail_rate, num_fail, num_pass)
        """

        group_on = group_on if group_on is not None else []

        for group_name in group_on:
            if group_name not in self._group_by:
                raise ValueError("Invalid group '{}' specified, the supported groups are {}"
                                 .format(group_name, self._group_by))

        tests = self._all_tests if "test" in group_on else [_ALL_TEST]
        tasks = self._all_tasks if "task" in group_on else [_ALL_TASK]
        variants = self._all_variants if "variant" in group_on else [_ALL_VARIANT]
        distros = self._all_distros if "distro" in group_on else [_ALL_DISTRO]

        group_lists = [tests, tasks, variants, distros]
        group_combos = list(itertools.product(*group_lists))
        view_report = []
        for group in group_combos:
            test_filter = group[0] if group[0] else _ALL_TEST
            task_filter = group[1] if group[1] else _ALL_TASK
            variant_filter = group[2] if group[2] else _ALL_VARIANT
            distro_filter = group[3] if group[3] else _ALL_DISTRO
            report_key = self.SummaryGroup(
                test=test_filter,
                task=task_filter,
                variant=variant_filter,
                distro=distro_filter,
                start_date=start_date,
                end_date=end_date)
            if report_key in self._summary_report:
                view_report.append(self.Summary(
                    test=test_filter if test_filter != _ALL_TEST else None,
                    task=task_filter if task_filter != _ALL_TASK else None,
                    variant=variant_filter if variant_filter != _ALL_VARIANT else None,
                    distro=distro_filter if distro_filter != _ALL_DISTRO else None,
                    start_date=start_date if start_date != _ALL_DATE else None,
                    end_date=end_date if end_date != _ALL_DATE else None,
                    fail_rate=self._summary_report[report_key]["fail_rate"],
                    num_fail=self._summary_report[report_key]["num_fail"],
                    num_pass=self._summary_report[report_key]["num_pass"]))
        return view_report


class HistoryReport(object):
    """The HistoryReport class interacts with the Evergreen REST API to generate a history_report.
       The history_report is meant to be viewed from the ViewReport class methods."""

    HistoryReportTuple = collections.namedtuple(
        "Report", "test task variant distro start_dt test_status")

    # TODO EVG-1653: Uncomment this line once the --sinceDate and --untilDate options are exposed.
    # period_types = ["date", "revision"]
    period_types = ["revision"]

    def __init__(self,
                 period_type,
                 start,
                 end,
                 start_day_of_week="first_day",
                 group_period=7,
                 project=_PROJECT,
                 tests=None,
                 tasks=None,
                 variants=None,
                 distros=None,
                 evg_cfg=None):
        # Initialize the report and object variables.
        self._report_tuples = []
        self._report = {"tests": {}}
        self.period_type = period_type.lower()
        if self.period_type not in self.period_types:
            raise ValueError(
                "Invalid time period type '{}' specified."
                " supported types are {}.".format(self.period_type, self.period_types))
        self.group_period = group_period
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

    @staticmethod
    def normalize_test_file(test_file):
        """Normalizes the test_file name:
           - Changes single backslash (\\) to forward slash (/)
           - Removes .exe extension
           Returns normalized string."""
        return test_file.replace("\\", "/").replace(".exe", "")

    def generate_report(self):
        """Creates detail for self._report from specified test history options.
           Returns a ViewReport object of self._report."""

        if self.period_type == "date":
            report_method = self._get_history_by_date
        else:
            report_method = self._get_history_by_revision

        self.tests = self._all_tests()

        rest_api_report = report_method(test_statuses=["fail", "pass"])

        for record in rest_api_report:
            # Save API record as namedtuple
            self._report_tuples.append(
                self.HistoryReportTuple(
                    test=str(HistoryReport.normalize_test_file(record["test_file"])),
                    task=str(record["task_name"]),
                    variant=str(record["variant"]),
                    distro=record.get("distro", _ALL_DISTRO),
                    start_dt=datestr_to_date(record["start_time"]),
                    test_status=record["test_status"]))

        return ViewReport(history_report=self._report_tuples,
                          group_period=self.group_period,
                          start_day_of_week=self.start_day_of_week)


def main():

    parser = optparse.OptionParser(description=__doc__,
                                   usage="Usage: %prog [options] test1 test2 ...")

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
                      type="int",
                      default=7,
                      help="Set group period days, defaults to '%default'.")

    parser.add_option("--weekStartDay", dest="start_day_of_week",
                      choices=["sunday", "monday", "first_day"],
                      default="first_day",
                      help="The group starting day of week, when --groupPeriod is not 1. "
                           " Set to 'sunday', 'monday' or 'first_day'."
                           " If 'first_day', the group will start on the first day of the"
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

    (options, tests) = parser.parse_args()

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

    if not options.tasks and not tests:
        parser.print_help()
        parser.error("Must specify either --tasks or at least one test")

    report = HistoryReport(period_type=period_type,
                           start=start,
                           end=end,
                           group_period=options.group_period,
                           start_day_of_week=options.start_day_of_week,
                           project=options.project,
                           tests=tests,
                           tasks=options.tasks.split(","),
                           variants=options.variants.split(","),
                           distros=options.distros.split(","),
                           evg_cfg=read_evg_config())
    view_report = report.generate_report()
    summ_report = view_report.view_summary(group_on=["test", "task", "variant"])
    for s in sorted(summ_report):
        print(s)

if __name__ == "__main__":
    main()
