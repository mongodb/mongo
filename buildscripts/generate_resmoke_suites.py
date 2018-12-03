#!/usr/bin/env python
"""
Resmoke Test Suite Generator.

Analyze the evergreen history for tests run under the given task and create new evergreen tasks
to attempt to keep the task runtime under a specified amount.
"""

from __future__ import absolute_import

import argparse
import datetime
import itertools
import logging
import os
import sys

from collections import defaultdict
from collections import namedtuple
from operator import itemgetter

from jinja2 import Template

from client.github import GithubApi

import client.evergreen as evergreen
import util.testname as testname
import util.time as timeutil

LOGGER = logging.getLogger(__name__)

TEMPLATES_DIR = "buildscripts/templates/generate_resmoke_suites"
TEST_SUITE_DIR = "buildscripts/resmokeconfig/suites"

MAX_RUNTIME_KEY = "max_runtime"

CommitRange = namedtuple("CommitRange", ["start", "end"])
ProjectTarget = namedtuple("ProjectTarget", ["owner", "project", "branch"])
Dependencies = namedtuple("Dependencies", ["evergreen", "github"])


def enable_logging():
    """Enable verbose logging for execution."""

    logging.basicConfig(
        format='[%(asctime)s - %(name)s - %(levelname)s] %(message)s',
        level=logging.DEBUG,
        stream=sys.stdout,
    )


def get_start_and_end_commit_since_date(github_api, target, start_date):
    """Get the first and last commits on the given branch from the start date specified."""

    params = {
        "since": "{:%Y-%m-%d}T00:00:00Z".format(start_date),
        "sha": target.branch,
    }

    commits = github_api.get_commits(target.owner, target.project, params)

    return CommitRange(commits[-1]["sha"], commits[0]["sha"])


def get_history_by_revision(evergreen_api, task, commit_range, evg_project, variants):
    """Call to the evergreen API to get the test history for the specified task."""

    params = {
        "sort": "latest",
        "tasks": task,
        "afterRevision": commit_range.start,
        "beforeRevision": commit_range.end,
        "testStatuses": "pass",
        "taskStatuses": "success",
    }

    if variants:
        params["variants"] = variants

    LOGGER.debug("evergreen get_history, params=%s", params)

    return evergreen_api.get_history(evg_project, params)


def get_test_history(evergreen_api, target, task, commit_range, variants):
    """Get test history from evergreen, repeating if the data was paginated."""

    evg_project = evergreen.generate_evergreen_project_name(target.owner, target.project,
                                                            target.branch)
    test_history = []
    iteration = 0
    while commit_range.start != commit_range.end:
        history = get_history_by_revision(evergreen_api, task, commit_range, evg_project, variants)
        LOGGER.debug("test_history[%d]=%d, commit_range=%s", iteration, len(history), commit_range)

        if not history:
            break

        test_history += history

        # The first test will have the latest revision for this result set because
        # get_history_by_revision() sorts by "latest".
        commit_range = CommitRange(history[0]["revision"], commit_range.end)

        LOGGER.debug("commit_range=%s", commit_range)
        iteration += 1

    return test_history


def split_hook_runs_out(executions):
    """Split the list of executions into a list of test executions and list of hook executions."""

    def is_execution_a_hook(execution):
        """Is the given execution object for a test hook."""

        test_file = testname.normalize_test_file(execution["test_file"])
        return testname.is_resmoke_hook(test_file)

    test_executions = [e for e in executions if not is_execution_a_hook(e)]
    hook_executions = [e for e in executions if is_execution_a_hook(e)]

    return test_executions, hook_executions


def group_by_attribute(list_to_group, attrib):
    """Sort and group a given list by the specified attribute."""

    return itertools.groupby(sorted(list_to_group, key=itemgetter(attrib)), key=itemgetter(attrib))


def organize_hooks(executions):
    """Organize the duration of hooks into a dictionary."""
    hooks = defaultdict(lambda: defaultdict(lambda: defaultdict(int)))

    for rev, rev_group in group_by_attribute(executions, "revision"):
        for variant, var_group in group_by_attribute(rev_group, "variant"):
            for hook in var_group:
                name = testname.split_test_hook_name(hook["test_file"])[0]
                hooks[rev][variant][name] += hook["duration"]

    return hooks


def execution_runtime(test_file, execution, hooks):
    """Calculate the runtime for the given execution."""

    rev = execution["revision"]
    variant = execution["variant"]
    runtime = timeutil.ns2sec(execution["duration"])
    possible_hook_name = testname.get_short_name_from_test_file(test_file)
    if rev in hooks and variant in hooks[rev] and possible_hook_name in hooks[rev][variant]:
        runtime += timeutil.ns2sec(hooks[rev][variant][possible_hook_name])

    return runtime


def organize_executions_by_test(executions):
    """Organize the list of test executions into a dictionary execution data about each test."""

    (test_executions, hook_executions) = split_hook_runs_out(executions)

    hooks = organize_hooks(hook_executions)

    def group_by_test_name(test_list):
        """Group the given test list by name."""

        def key_function(execution):
            """Get the normalized test_file for the given execution."""

            return testname.normalize_test_file(execution["test_file"])

        return itertools.groupby(sorted(test_list, key=key_function), key=key_function)

    tests = defaultdict(lambda: defaultdict(int))

    for test_file, tf_group in group_by_test_name(test_executions):
        # Only include test files that exist (a test file could have recently been deleted)
        if os.path.isfile(test_file):
            for variant, variant_group in group_by_attribute(tf_group, "variant"):
                runs = [execution_runtime(test_file, e, hooks) for e in variant_group]
                ave_execution_time = average_of_array(runs)
                tests[test_file][variant] = ave_execution_time

                if ave_execution_time > tests[test_file][MAX_RUNTIME_KEY]:
                    tests[test_file][MAX_RUNTIME_KEY] = ave_execution_time

    return tests


def average_of_array(array):
    """Calculate the average value in the given array."""
    total = sum(array)
    count = len(array)

    return total / count


def sort_list_of_test_by_max_runtime(tests):
    """Return a list of tests sorted by descending average runtime."""
    return sorted(tests.keys(), key=lambda test: tests[test][MAX_RUNTIME_KEY], reverse=True)


def divide_remaining_tests_among_suites(remaining_tests, tests, suites):
    """Divide the list of tests given among the suites given."""
    suite_idx = 0
    for test_name in remaining_tests:
        test = tests[test_name]
        current_suite = suites[suite_idx]
        current_suite.add_test(test_name, test)
        suite_idx += 1
        if suite_idx >= len(suites):
            suite_idx = 0


def divide_tests_into_suites_by_maxtime(tests, sorted_tests, max_time_seconds, max_suites=None):
    """
    Divide the given tests into suites.

    Each suite should be able to execute in less than the max time specified.
    """
    suites = []
    current_suite = Suite()
    last_test_processed = len(sorted_tests)
    LOGGER.debug("Determines suites for runtime: %ds", max_time_seconds)
    for idx, test_name in enumerate(sorted_tests):
        test = tests[test_name]
        if current_suite.get_runtime() + test[MAX_RUNTIME_KEY] > max_time_seconds:
            LOGGER.debug("Runtime(%d) + new test(%d) > max(%d)", current_suite.get_runtime(),
                         test[MAX_RUNTIME_KEY], max_time_seconds)
            if current_suite.get_test_count() > 0:
                suites.append(current_suite)
                current_suite = Suite()
                if max_suites and len(suites) >= max_suites:
                    last_test_processed = idx
                    break

        current_suite.add_test(test_name, test)

    if current_suite.get_test_count() > 0:
        suites.append(current_suite)

    if max_suites and last_test_processed < len(sorted_tests):
        # We must have hit the max suite limit, just randomly add the remaining tests to suites.
        divide_remaining_tests_among_suites(sorted_tests[last_test_processed:], tests, suites)

    return suites


def get_misc_model(test_list, extra_model_data=None):
    """Build a model that will run any missing tests."""
    model = {
        "is_misc": True,
        "excluded_tests": test_list,
    }

    if extra_model_data:
        model.update(extra_model_data)

    return model


def render_template(model, task, index):
    """Render the specified model as a yml file in the test suites directory."""
    template_file = "{dir}/{task}.yml.j2".format(dir=TEMPLATES_DIR, task=task)
    target_file = "{dir}/{task}_{index}.yml".format(dir=TEST_SUITE_DIR, task=task, index=index)

    render(model, template_file, target_file)


def render(model, source, destination):
    """Render the specified model with the template at `source` to the file `destination`."""
    with open(source, "r") as inp, open(destination, "w") as out:
        template = Template(inp.read(), trim_blocks=True)
        out.write(template.render(model))


class Suite(object):
    """A suite of tests that can be run by evergreen."""

    def __init__(self):
        """Initialize the object."""
        self.tests = []
        self.total_runtime = 0
        self.variant_runtime = defaultdict(int)

    def add_test(self, test_name, test_data):
        """Add the given test to this suite."""

        self.tests.append(test_name)
        for variant in test_data:
            if variant == MAX_RUNTIME_KEY:
                self.total_runtime += test_data[variant]
            else:
                self.variant_runtime[variant] += test_data[variant]

    def get_runtime(self):
        """Get the current average runtime of all the tests currently in this suite."""

        return self.total_runtime

    def get_test_count(self):
        """Get the number of tests currently in this suite."""

        return len(self.tests)

    def get_model(self, extra_model_data=None):
        """Get a model of this suite that can be used to render a yml file."""

        model = {"test_names": self.tests, "variants": []}
        for variant in self.variant_runtime:
            model["variants"].append(
                {"name": variant, "runtime": self.variant_runtime[variant] / 60})

        if extra_model_data:
            model.update(extra_model_data)

        return model


class Main(object):
    """Orchestrate the execution of generate_resmoke_suites."""

    def __init__(self, deps):
        """Initialize the object."""
        self.deps = deps
        self.options = {}
        self.commit_range = None
        self.test_list = []

    def parse_commandline(self):
        """Parse the command line options and return the parsed data."""
        parser = argparse.ArgumentParser(description=self.main.__doc__)

        parser.add_argument("--analysis-duration", dest="duration_days", default=14,
                            help="Number of days to analyze.")
        parser.add_argument("--branch", dest="branch", default="master",
                            help="Branch of project to analyze.")
        parser.add_argument("--end-commit", dest="end_commit", help="End analysis at this commit.")
        parser.add_argument("--execution-time", dest="execution_time_minutes", default=60, type=int,
                            help="Target execution time (in minutes).")
        parser.add_argument("--github-owner", dest="owner", default="mongodb",
                            help="Owner of github project to analyse.")
        parser.add_argument("--github-project", dest="project", default="mongo",
                            help="Github project to analyse.")
        parser.add_argument("--start-commit", dest="start_commit",
                            help="Start analysis at this commit.")
        parser.add_argument("--variants", dest="variants", metavar="<variant1,variant2,...>",
                            default=None,
                            help="Comma-separated list of Evergreeen build variants to analyze.")
        parser.add_argument("--max-sub-suites", dest="max_sub_suites", type=int,
                            help="Max number of suites to divide into.")
        parser.add_argument("--verbose", dest="verbose", action="store_true", default=False,
                            help="Enable verbose logging.")
        parser.add_argument("task", nargs=1, help="task to analyze.")

        options = parser.parse_args()

        if options.start_commit or options.end_commit:
            if not options.start_commit or not options.end_commit:
                parser.error("--start-commit and --end-commit must both be specified")

        return options

    def get_data(self, target, start_date, task, variants):
        """Collect history test data from github and evergreen."""
        if not self.commit_range:
            self.commit_range = get_start_and_end_commit_since_date(self.deps.github, target,
                                                                    start_date)
        return get_test_history(self.deps.evergreen, target, task, self.commit_range, variants)

    def calculate_suites(self, data, execution_time_secs):
        """Divide test into suites that can be run in less than the specified execution time."""
        tests = organize_executions_by_test(data)
        self.test_list = sort_list_of_test_by_max_runtime(tests)
        return divide_tests_into_suites_by_maxtime(tests, self.test_list, execution_time_secs,
                                                   self.options.max_sub_suites)

    def render_suites(self, suites, task):
        """Render the given suites into yml files that can be used by resmoke.py."""
        for idx, suite in enumerate(suites):
            render_template(suite.get_model(self.extra_model_data()), task, idx)

    def render_misc_suite(self, task):
        """Render a misc suite to run any tests that might be added to the task directory."""
        model = get_misc_model(self.test_list, self.extra_model_data())
        source = "{dir}/{task}.yml.j2".format(dir=TEMPLATES_DIR, task=task)
        target = "{dir}/{task}_misc.yml".format(dir=TEST_SUITE_DIR, task=task)

        render(model, source, target)

    def extra_model_data(self):
        """Build extra data to include in the model."""
        return {
            "options": self.options,
            "start_commit": self.commit_range.start,
            "end_commit": self.commit_range.end,
        }

    def main(self):
        """Generate resmoke suites that run within a specified target execution time."""

        options = self.parse_commandline()

        self.options = options

        if options.verbose:
            enable_logging()

        if options.start_commit or options.end_commit:
            self.commit_range = CommitRange(options.start_commit, options.end_commit)

        LOGGER.debug("Starting execution for options %s", options)
        task = options.task[0]

        today = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = today - datetime.timedelta(days=options.duration_days)

        target = ProjectTarget(options.owner, options.project, options.branch)

        data = self.get_data(target, start_date, task, options.variants)
        suites = self.calculate_suites(data, options.execution_time_minutes * 60)

        LOGGER.debug("Creating %d suites", len(suites))

        self.render_suites(suites, task)
        self.render_misc_suite(task)


if __name__ == "__main__":
    Main(Dependencies(evergreen.get_evergreen_api(), GithubApi())).main()
