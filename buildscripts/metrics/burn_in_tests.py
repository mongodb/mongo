"""Metric tracking script for burn_in_tests."""
from __future__ import absolute_import

import argparse
import datetime
import json
import logging
import os
import sys

import requests

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from buildscripts.client import evergreen as evg_client  # pylint: disable=wrong-import-position

LOGGER = logging.getLogger(__name__)
LOG_LEVELS = ["DEBUG", "INFO", "WARNING", "ERROR"]

BURN_IN_TESTS_TASK = "burn_in_tests"
BURN_IN_GENERATED_TASK_PREFIX = "burn_in:"
# Burn_in_tests are expected to run no more than 600 seconds (10 minutes). Overhead to start and
# stop the test framework is not included and the task time for each single test can exceed this
# amount. We add 60 seconds of overhead (10%) to the expected time of the burn_in_tests task for
# test setup and teardown.
BURN_IN_TIME_SEC = 600 + 60
BURN_IN_TIME_MS = BURN_IN_TIME_SEC * 1000
BURN_IN_TASKS_EXCEED = "burn_in_tasks_exceeding_{}s".format(BURN_IN_TIME_SEC)

DATE_FORMAT = "%Y-%m-%dT%H:%M:%S.%fZ"
DEFAULT_PROJECT = "mongodb-mongo-master"
DEFAULT_DAYS = 28

DEFAULT_REPORT_FILE = "burn_in_tests_metrics.json"
REPORT_COMMENT_FIELD = "_comment"
REPORT_TIME_FIELDS = ["report_start_time", "report_end_time"]
REPORT_COUNTER_FIELDS = [
    "patch_builds_with_burn_in_task", "tasks", "tasks_succeeded", "tasks_failed",
    "tasks_failed_burn_in", "tasks_failed_only_burn_in", "burn_in_generated_tasks", "burn_in_tests",
    BURN_IN_TASKS_EXCEED
]
REPORT_FIELDS = REPORT_TIME_FIELDS + REPORT_COUNTER_FIELDS


def parse_command_line():
    """Parse command line options.

    :return: Argparser object.
    """

    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument(
        "--days", dest="days", default=DEFAULT_DAYS, type=int,
        help="The number of days from today for the report. Default is '%(default)d'.")
    parser.add_argument("--project", dest="project", default=DEFAULT_PROJECT,
                        help="The name of the Evergreen project. Default is '%(default)s'.")
    parser.add_argument("--reportFile", dest="report_file", default=DEFAULT_REPORT_FILE,
                        help="Output report JSON file. Default is '%(default)s'.")
    parser.add_argument("--logLevel", dest="log_level", default=None, choices=LOG_LEVELS,
                        help="Set the log level.")
    parser.add_argument("--evgClientLogLevel", dest="evg_client_log_level", default=None,
                        choices=LOG_LEVELS, help="Set the Evergreen client log level.")
    return parser


def configure_logging(log_level, evg_client_log_level):
    """Enable logging for execution.

    :param log_level: Set the log level of this script.
    :param evg_client_log_level: Set the log level of the Evergreen client methods.
    """
    if log_level:
        logging.basicConfig(format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s")
        LOGGER.setLevel(log_level)
    if evg_client_log_level:
        evg_client.LOGGER.setLevel(evg_client_log_level)


def write_json_file(json_data, pathname):
    """Write out a JSON file.

    :param json_data: Dict to save in JSON format.
    :param pathname: Output file to save as JSON.
    """
    with open(pathname, "w") as fstream:
        json.dump(json_data, fstream, indent=4, sort_keys=True)


def str_to_datetime(time_str):
    """Return datetime from time_str.

    :param time_str: Time string conforming to DATE_FORMAT format.
    :return: dateteime.datetime object.
    """
    return datetime.datetime.strptime(time_str, DATE_FORMAT)


def str_to_datetime_date(time_str):
    """Return datetime.date() from time_str.

    :param time_str: Time string conforming to DATE_FORMAT format.
    :return: dateteime.datetime.date() object.
    """
    return str_to_datetime(time_str).date()


def get_burn_in_builds(evg_api, project, days):
    """List of builds from patches with a burn_in_tests task.

    :param evg_api: The Evergreen client API instance.
    :param project: The name of the project.
    :param days: Number of days to go backwards to search for patches.
    :return: List of version builds with a burn_in_tests task.
    """
    burn_in_builds = []
    end_date = (datetime.datetime.utcnow() - datetime.timedelta(days=days)).date()
    for patch in evg_api.project_patches_gen(project):
        if str_to_datetime_date(patch["create_time"]) < end_date:
            break
        # Skip patch builds with a created status, as they may not yet have an associated build,
        # and are therefore not ready to be analyzed.
        if patch["status"] == "created":
            continue
        for build in evg_api.version_builds(patch["patch_id"]):
            for task in build["tasks"]:
                if BURN_IN_TESTS_TASK in task:
                    burn_in_builds.append(build)
                    break

    return burn_in_builds


def is_burn_in_display_task(display_name):
    """Return True if display_name is a burn_in task.

    :param display_name: Task display name
    :return: Boolean, True if name is a burn_in task.
    """
    return display_name == BURN_IN_TESTS_TASK or display_name.startswith(
        BURN_IN_GENERATED_TASK_PREFIX)


def get_burn_in_tasks(evg_api, builds):
    """List of burn_in tasks from the builds.

    :param evg_api: The Evergreen client API instance.
    :param patches: List of builds.
    :return: List of build tasks.
    """
    tasks = []
    for build in builds:
        for build_task in evg_api.tasks_by_build_id(build["_id"]):
            if is_burn_in_display_task(build_task["display_name"]):
                tasks.append(build_task)
    return tasks


def get_tests_from_tasks(evg_api, tasks):
    """List of tests from the tasks.

    :param evg_api: The Evergreen client API instance.
    :param patches: List of tasks.
    :return: List of tests.
    """
    tests = []
    for task in tasks:
        try:
            task_tests = evg_api.tests_by_task(task["task_id"], task["execution"])
            tests.extend(task_tests)
        except requests.exceptions.HTTPError as err:
            # Not all tasks have associated tests, so we ignore HTTP 404 errors.
            if err.response.status_code != 404:
                raise err
    return tests


class Report(object):
    """Report class to provide burn_in_tests report."""

    def __init__(self, builds, tasks, tests, comment=None):
        """Initialize report class.

        :param builds: List of burn_in builds.
        :param tasks: List of burn_in tasks.
        :param tests: List of burn_in tests.
        :param comment: Optional comment field added to the report.
        """
        self.burn_in_patch_builds = self._init_burn_in_patch_builds(builds)
        self.burn_in_tasks = self._init_burn_in_tasks(tasks)
        num_burn_in_tasks = len([
            task for task in tasks if task["display_name"].startswith(BURN_IN_GENERATED_TASK_PREFIX)
        ])
        self.report = self._init_report_fields(
            len(self.burn_in_patch_builds), num_burn_in_tasks, len(tests), comment)

    @staticmethod
    def _init_burn_in_patch_builds(builds):
        """Group builds into a dict keyed by the patch version.

        :param: builds: List of burn_in builds.
        :return Dict of builds grouped by patch version.
        """
        burn_in_patch_builds = {}
        for build in builds:
            build_version = build["version"]
            if build_version not in burn_in_patch_builds:
                burn_in_patch_builds[build_version] = {"builds": []}
            burn_in_patch_builds[build_version]["builds"].append(build)
        return burn_in_patch_builds

    @staticmethod
    def _init_burn_in_tasks(tasks):
        """Convert tasks into a dict for direct access by task_id.

        :param tasks: List of burn_in tasks.
        :return Dict of tasks keyed by task_id.
        """
        return {task["task_id"]: task for task in tasks}

    @staticmethod
    def _init_report_fields(num_patch_builds, num_tasks, num_tests, comment=None):
        """Init the report fields.

        :param num_patch_builds: Number of patch builds.
        :param num_tasks: Number of of burn_in tasks.
        :param num_tests: Number of burn_in tests.
        :param comment: Optional comment field added to the report.
        :return Dict of report fields.
        """
        report = {}
        if comment:
            report[REPORT_COMMENT_FIELD] = comment
        for field in REPORT_TIME_FIELDS:
            report[field] = None
        for field in REPORT_COUNTER_FIELDS:
            report[field] = 0
        report["patch_builds_with_burn_in_task"] = num_patch_builds
        report["burn_in_generated_tasks"] = num_tasks
        report["burn_in_tests"] = num_tests
        return report

    def _update_report_time(self, create_time):
        """Update report start_time and end_time.

        :param create_time: The time used to compare against the report start and end time.
        """
        start_time = self.report["report_start_time"]
        end_time = self.report["report_end_time"]
        create_dt = str_to_datetime(create_time)
        if not start_time or create_dt < str_to_datetime(start_time):
            self.report["report_start_time"] = create_time
        if not end_time or create_dt > str_to_datetime(end_time):
            self.report["report_end_time"] = create_time

    def _update_report_burn_in(self, patch_builds, total_failures):
        """Update burn_in portion of report.

        :param patch_builds: List of builds in the patch.
        :param total_failures: Number of total failures for the patch.
        """
        burn_in_failures = 0
        for build in patch_builds:
            for task_id in build["tasks"]:
                burn_in_task = task_id in self.burn_in_tasks
                if not burn_in_task:
                    continue
                if self.burn_in_tasks[task_id]["status"] == "failed":
                    self.report["tasks_failed_burn_in"] += 1
                    burn_in_failures += 1
                if self.burn_in_tasks[task_id]["time_taken_ms"] > BURN_IN_TIME_MS:
                    self.report[BURN_IN_TASKS_EXCEED] += 1
        if self._is_patch_build_completed(
                patch_builds) and burn_in_failures > 0 and burn_in_failures == total_failures:
            LOGGER.debug("Patch build %s failed only burn_in_tests with %d failures",
                         patch_builds[0]["version"], total_failures)
            self.report["tasks_failed_only_burn_in"] += 1

    def _update_report_status(self, build):
        """Update task status of report.

        :param build: The build object to analyze.
        """
        self.report["tasks"] += len(build["tasks"])
        self.report["tasks_succeeded"] += build["status_counts"]["succeeded"]
        self.report["tasks_failed"] += build["status_counts"]["failed"]

    @staticmethod
    def _is_patch_build_completed(builds):
        """Return True if all builds are completed.

        :param builds: List of builds.
        :return: True if build status is 'failed' or 'success' for all builds.
        """
        return all([build["status"] in ["failed", "success"] for build in builds])

    def generate_report(self):
        """Generate report metrics for burn_in_tests task.

        :return: Dict of report.
        """
        for patch_build in self.burn_in_patch_builds.values():
            build_failed_tasks = 0
            for build in patch_build["builds"]:
                self._update_report_time(build["create_time"])
                self._update_report_status(build)
                build_failed_tasks += build["status_counts"]["failed"]
            self._update_report_burn_in(patch_build["builds"], build_failed_tasks)

        return self.report


def main():
    """Execute Main program."""

    options = parse_command_line().parse_args()
    configure_logging(options.log_level, options.evg_client_log_level)
    evg_api = evg_client.EvergreenApiV2(api_headers=evg_client.get_evergreen_headers())

    LOGGER.info("Getting the patch version builds")
    burn_in_builds = get_burn_in_builds(evg_api, options.project, options.days)

    LOGGER.info("Getting the build tasks")
    burn_in_tasks = get_burn_in_tasks(evg_api, burn_in_builds)

    LOGGER.info("Getting the task tests")
    burn_in_tests = get_tests_from_tasks(evg_api, burn_in_tasks)

    comment = ("Metrics for patch builds running burn_in_tests in {} for the last {} days -"
               " generated on {}Z").format(options.project, options.days,
                                           datetime.datetime.utcnow().isoformat())

    report = Report(burn_in_builds, burn_in_tasks, burn_in_tests, comment=comment)
    report_result = report.generate_report()
    write_json_file(report_result, options.report_file)
    LOGGER.info("%s", report_result)


if __name__ == "__main__":
    main()
