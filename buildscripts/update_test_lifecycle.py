#!/usr/bin/env python
"""Test Failures module.

Update etc/test_lifecycle.yml to tag unreliable tests based on historic failure rates.
"""

from __future__ import absolute_import
from __future__ import division

import collections
import datetime
import logging
import multiprocessing.dummy
import operator
import optparse
import os.path
import posixpath
import subprocess
import sys
import textwrap
import warnings

import yaml

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts import git
from buildscripts import jiraclient
from buildscripts import resmokelib
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.utils import globstar
from buildscripts import lifecycle_test_failures as tf
from buildscripts.ciconfig import evergreen as ci_evergreen
from buildscripts.ciconfig import tags as ci_tags
# pylint: enable=wrong-import-position

# pylint: disable=too-many-lines

LOGGER = logging.getLogger(__name__)

if sys.version_info[0] == 2:
    _NUMBER_TYPES = (int, long, float)
else:
    _NUMBER_TYPES = (int, float)

Rates = collections.namedtuple("Rates", ["acceptable", "unacceptable"])

Config = collections.namedtuple("Config", [
    "test_fail_rates",
    "task_fail_rates",
    "variant_fail_rates",
    "distro_fail_rates",
    "reliable_min_runs",
    "reliable_time_period",
    "unreliable_min_runs",
    "unreliable_time_period",
])

DEFAULT_CONFIG = Config(
    test_fail_rates=Rates(acceptable=0.1, unacceptable=0.3), task_fail_rates=Rates(
        acceptable=0.1, unacceptable=0.3),
    variant_fail_rates=Rates(acceptable=0.2, unacceptable=0.4), distro_fail_rates=Rates(
        acceptable=0.2,
        unacceptable=0.4), reliable_min_runs=5, reliable_time_period=datetime.timedelta(weeks=1),
    unreliable_min_runs=20, unreliable_time_period=datetime.timedelta(weeks=4))

DEFAULT_PROJECT = "mongodb-mongo-master"

DEFAULT_NUM_THREADS = 12


def get_suite_tasks_membership(evg_conf):
    """Return a dictionary with keys of all suites and list of associated tasks."""
    suite_membership = collections.defaultdict(list)
    for task in evg_conf.tasks:
        suite = task.resmoke_suite
        if suite:
            suite_membership[suite].append(task.name)
    return suite_membership


def get_test_tasks_membership(evg_conf):
    """Return a dictionary with keys of all tests and list of associated tasks."""
    test_suites_membership = resmokelib.suitesconfig.create_test_membership_map(test_kind="js_test")
    suite_tasks_membership = get_suite_tasks_membership(evg_conf)
    test_tasks_membership = collections.defaultdict(list)
    for test in test_suites_membership.keys():
        for suite in test_suites_membership[test]:
            test_tasks_membership[test].extend(suite_tasks_membership[suite])
    return test_tasks_membership


def get_tests_from_tasks(tasks, test_tasks_membership):
    """Return a list of tests from list of specified tasks."""
    tests = []
    tasks_set = set(tasks)
    for test in test_tasks_membership.keys():
        if not tasks_set.isdisjoint(test_tasks_membership[test]):
            tests.append(test)
    return tests


def create_test_groups(tests):
    """Return groups of tests by their directory, i.e., jstests/core."""
    test_groups = collections.defaultdict(list)
    for test in tests:
        test_split = test.split("/")
        # If the test does not have a directory, then ignore it.
        if len(test_split) <= 1:
            continue
        test_dir = test_split[1]
        test_groups[test_dir].append(test)
    return test_groups


def create_batch_groups(test_groups, batch_size):
    """Return batch groups list of test_groups."""
    batch_groups = []
    for test_group_name in test_groups:
        test_group = test_groups[test_group_name]
        while test_group:
            batch_groups.append(test_group[:batch_size])
            test_group = test_group[batch_size:]
    return batch_groups


class TestHistorySource(object):
    """A class used to parallelize requests to buildscripts.test_failures.TestHistory."""

    def __init__(  # pylint: disable=too-many-arguments
            self, project, variants, distros, start_revision, end_revision,
            thread_pool_size=DEFAULT_NUM_THREADS):
        """Initialize the TestHistorySource.

        Args:
            project: the Evergreen project name.
            variants: a list of variant names.
            distros: a list of distro names.
            start_revision: the revision delimiting the begining of the history we want to retrieve.
            end_revision: the revision delimiting the end of the history we want to retrieve.
            thread_pool_size: the size of the thread pool used to make parallel requests.
        """
        self._project = project
        self._variants = variants
        self._distros = distros
        self._start_revision = start_revision
        self._end_revision = end_revision
        self._thread_pool = multiprocessing.dummy.Pool(thread_pool_size)

    def get_history_data(self, tests, tasks):
        """Retrieve the history data for the given tests and tasks.

        The requests for each task will be parallelized using the internal thread pool.
        """
        history_data = []
        jobs = [
            self._thread_pool.apply_async(self._get_task_history_data, (tests, task))
            for task in tasks
        ]
        for job in jobs:
            history_data.extend(job.get())
        return history_data

    def _get_task_history_data(self, tests, task):
        test_history = tf.TestHistory(project=self._project, tests=tests, tasks=[task],
                                      variants=self._variants, distros=self._distros)
        return test_history.get_history_by_revision(start_revision=self._start_revision,
                                                    end_revision=self._end_revision)


def callo(args):
    """Call a program, and capture its output."""
    return subprocess.check_output(args)


def git_commit_range_since(since):
    """Return first and last commit in 'since' period specified.

    Specify 'since' as any acceptable period for git log --since.
    The period can be specified as '4.weeks' or '3.days'.
    """
    git_command = "git log --since={} --pretty=format:%H".format(since)
    commits = callo(git_command.split()).split("\n")
    return commits[-1], commits[0]


def git_commit_prior(revision):
    """Return commit revision prior to one specified."""
    git_format = "git log -2 {revision} --pretty=format:%H"
    git_command = git_format.format(revision=revision)
    commits = callo(git_command.split()).split("\n")
    return commits[-1]


def unreliable_test(test_fr, unacceptable_fr, test_runs, min_run):
    """Check for an unreliable test.

    A test should be added to the set of tests believed not to run reliably when it has more
    than min_run executions with a failure percentage greater than unacceptable_fr.
    """
    return test_runs >= min_run and test_fr >= unacceptable_fr


def reliable_test(test_fr, acceptable_fr, test_runs, min_run):
    """Check for a reliable test.

    A test should then removed from the set of tests believed not to run reliably when it has
    less than min_run executions or has a failure percentage less than acceptable_fr.
    """
    return test_runs < min_run or test_fr <= acceptable_fr


def check_fail_rates(fr_name, acceptable_fr, unacceptable_fr):
    """Raise an error if the acceptable_fr > unacceptable_fr."""
    if acceptable_fr > unacceptable_fr:
        raise ValueError("'{}' acceptable failure rate {} must be <= the unacceptable failure rate"
                         " {}".format(fr_name, acceptable_fr, unacceptable_fr))


def check_days(name, days):
    """Raise an error if days < 1."""
    if days < 1:
        raise ValueError("'{}' days must be greater than 0.".format(name))


def unreliable_tag(task, variant, distro):
    """Return the unreliable tag."""

    for (component_name, component_value) in (("task", task), ("variant", variant), ("distro",
                                                                                     distro)):
        if isinstance(component_value, (tf.Wildcard, tf.Missing)):
            if component_name == "task":
                return "unreliable"
            elif component_name == "variant":
                return "unreliable|{}".format(task)
            elif component_name == "distro":
                return "unreliable|{}|{}".format(task, variant)

    return "unreliable|{}|{}|{}".format(task, variant, distro)


def update_lifecycle(  # pylint: disable=too-many-arguments
        lifecycle_tags_file, report, method_test, add_tags, fail_rate, min_run):
    """Update the lifecycle object based on the test_method.

    The test_method checks unreliable or reliable fail_rates.
    """
    for summary in report:
        if method_test(summary.fail_rate, fail_rate, summary.num_pass + summary.num_fail, min_run):
            update_tag = unreliable_tag(summary.task, summary.variant, summary.distro)
            if add_tags:
                lifecycle_tags_file.add_tag("js_test", summary.test, update_tag, summary.fail_rate)
            else:
                lifecycle_tags_file.remove_tag("js_test", summary.test, update_tag,
                                               summary.fail_rate)


def compare_tags(tag_a, tag_b):
    """Return 1, -1 or 0 if 'tag_a' is superior, inferior or equal to 'tag_b'."""
    return cmp(tag_a.split("|"), tag_b.split("|"))


def validate_config(config):  # pylint: disable=too-many-branches
    """Raise a TypeError or ValueError exception if 'config' isn't a valid model."""

    for (name, fail_rates) in (("test", config.test_fail_rates), ("task", config.task_fail_rates),
                               ("variant", config.variant_fail_rates), ("distro",
                                                                        config.distro_fail_rates)):
        if not isinstance(fail_rates.acceptable, _NUMBER_TYPES):
            raise TypeError("The acceptable {} failure rate must be a number, but got {}".format(
                name, fail_rates.acceptable))
        elif fail_rates.acceptable < 0 or fail_rates.acceptable > 1:
            raise ValueError(("The acceptable {} failure rate must be between 0 and 1 (inclusive),"
                              " but got {}").format(name, fail_rates.acceptable))
        elif not isinstance(fail_rates.unacceptable, _NUMBER_TYPES):
            raise TypeError("The unacceptable {} failure rate must be a number, but got {}".format(
                name, fail_rates.unacceptable))
        elif fail_rates.unacceptable < 0 or fail_rates.unacceptable > 1:
            raise ValueError(("The unacceptable {} failure rate must be between 0 and 1"
                              " (inclusive), but got {}").format(name, fail_rates.unacceptable))
        elif fail_rates.acceptable > fail_rates.unacceptable:
            raise ValueError(
                ("The acceptable {0} failure rate ({1}) must be no larger than unacceptable {0}"
                 " failure rate ({2})").format(name, fail_rates.acceptable,
                                               fail_rates.unacceptable))

    for (name, min_runs) in (("reliable", config.reliable_min_runs), ("unreliable",
                                                                      config.unreliable_min_runs)):
        if not isinstance(min_runs, _NUMBER_TYPES):
            raise TypeError(("The minimum number of runs for considering a test {} must be a"
                             " number, but got {}").format(name, min_runs))
        elif min_runs <= 0:
            raise ValueError(("The minimum number of runs for considering a test {} must be a"
                              " positive integer, but got {}").format(name, min_runs))
        elif isinstance(min_runs, float) and not min_runs.is_integer():
            raise ValueError(("The minimum number of runs for considering a test {} must be an"
                              " integer, but got {}").format(name, min_runs))

    for (name, time_period) in (("reliable", config.reliable_time_period),
                                ("unreliable", config.unreliable_time_period)):
        if not isinstance(time_period, datetime.timedelta):
            raise TypeError(
                "The {} time period must be a datetime.timedelta instance, but got {}".format(
                    name, time_period))
        elif time_period.days <= 0:
            raise ValueError(
                "The {} time period must be a positive number of days, but got {}".format(
                    name, time_period))
        elif time_period - datetime.timedelta(days=time_period.days) > datetime.timedelta():
            raise ValueError(
                "The {} time period must be an integral number of days, but got {}".format(
                    name, time_period))


def _test_combination_from_entry(entry, components):
    """Create a test combination tuple from a tf._ReportEntry and target components.

    Return a tuple containing the entry fields specified in components.
    """
    combination = []
    for component in components:
        combination.append(operator.attrgetter(component)(entry))
    return tuple(combination)


def _test_combination_from_tag(test, tag):
    """Create a test combination tuple from a test name and a tag.

    Return a tuple containing the test name and the components found in the tag.
    """
    combination = [test]
    for element in _split_tag(tag):
        if element:
            combination.append(element)
    return tuple(combination)


def update_tags(lifecycle_tags, config, report, tests):  # pylint: disable=too-many-locals
    """Update the tags in 'lifecycle_tags'.

    This is based on the historical test failures of tests 'tests'
    mentioned in 'report' according to the model described by 'config'.
    """

    # We initialize 'grouped_entries' to make PyLint not complain about 'grouped_entries' being used
    # before assignment.
    grouped_entries = None
    # yapf: disable
    for (idx, (components, rates)) in enumerate(
            ((tf.Report.TEST_TASK_VARIANT_DISTRO, config.distro_fail_rates),
             (tf.Report.TEST_TASK_VARIANT, config.variant_fail_rates),
             (tf.Report.TEST_TASK, config.task_fail_rates),
             (tf.Report.TEST, config.test_fail_rates))):
        # yapf: enable
        if idx > 0:
            report = tf.Report(grouped_entries)

        # We reassign the value of 'grouped_entries' to take advantage of how data that is on
        # (test, task, variant, distro) preserves enough information to be grouped on any subset of
        # those components, etc.
        grouped_entries = report.summarize_by(components, time_period=tf.Report.DAILY)

        # Create the reliable report.
        # Filter out any test executions from prior to 'config.reliable_time_period'.
        reliable_start_date = (
            report.end_date - config.reliable_time_period + datetime.timedelta(days=1))
        reliable_entries = [
            entry for entry in grouped_entries if entry.start_date >= reliable_start_date
        ]
        if reliable_entries:
            reliable_report = tf.Report(reliable_entries)
            reliable_combinations = {
                _test_combination_from_entry(entry, components)
                for entry in reliable_entries
            }
            reliable_summaries = reliable_report.summarize_by(components)
        else:
            reliable_combinations = set()
            reliable_summaries = []

        # Create the unreliable report.
        # Filter out any test executions from prior to 'config.unreliable_time_period'.
        # Also filter out any test that is not present in the reliable_report in order
        # to avoid tagging as unreliable tests that are no longer running.
        unreliable_start_date = (
            report.end_date - config.unreliable_time_period + datetime.timedelta(days=1))
        unreliable_entries = [
            entry for entry in grouped_entries
            if (entry.start_date >= unreliable_start_date
                and _test_combination_from_entry(entry, components) in reliable_combinations)
        ]
        if unreliable_entries:
            unreliable_report = tf.Report(unreliable_entries)
            unreliable_summaries = unreliable_report.summarize_by(components)
        else:
            unreliable_summaries = []

        # Update the tags using the unreliable report.
        update_lifecycle(lifecycle_tags, unreliable_summaries, unreliable_test, True,
                         rates.unacceptable, config.unreliable_min_runs)

        # Update the tags using the reliable report.
        update_lifecycle(lifecycle_tags, reliable_summaries, reliable_test, False, rates.acceptable,
                         config.reliable_min_runs)

        def should_be_removed(test, tag, components, reliable_combinations):
            """Return True if 'combination' shoud be removed."""
            combination = _test_combination_from_tag(test, tag)
            if len(combination) != len(components):
                # The tag is not for these components.
                return False
            return combination not in reliable_combinations

        # Remove the tags that correspond to tests that have not run during the reliable period.
        for test in tests:
            tags = lifecycle_tags.lifecycle.get_tags("js_test", test)
            for tag in tags[:]:
                if should_be_removed(test, tag, components, reliable_combinations):
                    LOGGER.info("Removing tag '%s' of test '%s' because the combination did not run"
                                " during the reliable period", tag, test)
                    lifecycle_tags.remove_tag("js_test", test, tag, failure_rate=0)


def _split_tag(tag):
    """Split a tag into its components.

    Return a tuple containing task, variant, distro. The values are None if absent from the tag.
    If the tag is invalid, the return value is (None, None, None).
    """
    elements = tag.split("|")
    length = len(elements)
    if elements[0] != "unreliable" or length < 2 or length > 4:
        return None, None, None
    # fillout the array
    elements.extend([None] * (4 - length))
    # return as a tuple
    return tuple(elements[1:])


def _is_tag_still_relevant(evg_conf, tag):
    """Indicate if a tag still corresponds to a valid task/variant/distro combination."""
    if tag == "unreliable":
        return True
    task, variant, distro = _split_tag(tag)
    if not task or task not in evg_conf.task_names:
        return False
    if variant:
        variant_conf = evg_conf.get_variant(variant)
        if not variant_conf or task not in variant_conf.task_names:
            return False
        if distro and distro not in variant_conf.distros:
            return False
    return True


def clean_up_tags(lifecycle_tags, evg_conf):
    """Remove the tags that do not correspond to a valid test/task/variant/distro combination."""
    lifecycle = lifecycle_tags.lifecycle
    for test_kind in lifecycle.get_test_kinds():
        for test_pattern in lifecycle.get_test_patterns(test_kind):
            if not globstar.glob(test_pattern):
                # The pattern does not match any file in the repository.
                lifecycle_tags.clean_up_test(test_kind, test_pattern)
                continue
            for tag in lifecycle.get_tags(test_kind, test_pattern):
                if not _is_tag_still_relevant(evg_conf, tag):
                    lifecycle_tags.clean_up_tag(test_kind, test_pattern, tag)


def _config_as_options(config):
    return ("--reliableTestMinRuns {} "
            "--reliableDays {} "
            "--unreliableTestMinRuns {} "
            "--unreliableDays {} "
            "--testFailRates {} {} "
            "--taskFailRates {} {} "
            "--variantFailRates {} {} "
            "--distroFailRates {} {}").format(
                config.reliable_min_runs, config.reliable_time_period.days,
                config.unreliable_min_runs, config.unreliable_time_period.days,
                config.test_fail_rates.acceptable, config.test_fail_rates.unacceptable,
                config.task_fail_rates.acceptable, config.task_fail_rates.unacceptable,
                config.variant_fail_rates.acceptable, config.variant_fail_rates.unacceptable,
                config.distro_fail_rates.acceptable, config.distro_fail_rates.unacceptable)


class TagsConfigWithChangelog(object):
    """A wrapper around TagsConfig to update a tags file and record the modifications made."""

    def __init__(self, lifecycle):
        """Initialize the TagsConfigWithChangelog with the lifecycle TagsConfig."""
        self.lifecycle = lifecycle
        self.added = {}
        self.removed = {}
        self.cleaned_up = {}

    @staticmethod
    def _cancel_tag_log(log_dict, test_kind, test, tag):
        """Remove a tag from a changelog dictionary.

        Used to remove a tag from the 'added' or 'removed' attribute.
        """
        kind_dict = log_dict[test_kind]
        test_dict = kind_dict[test]
        del test_dict[tag]
        if not test_dict:
            del kind_dict[test]
            if not kind_dict:
                del log_dict[test_kind]

    def add_tag(self, test_kind, test, tag, failure_rate):
        """Add a tag."""
        if self.lifecycle.add_tag(test_kind, test, tag):
            if tag in self.removed.get(test_kind, {}).get(test, {}):
                # The tag has just been removed.
                self._cancel_tag_log(self.removed, test_kind, test, tag)
            else:
                self.added.setdefault(test_kind, {}).setdefault(test, {})[tag] = failure_rate

    def remove_tag(self, test_kind, test, tag, failure_rate):
        """Remove a tag."""
        if self.lifecycle.remove_tag(test_kind, test, tag):
            if tag in self.added.get(test_kind, {}).get(test, {}):
                # The tag has just been added.
                self._cancel_tag_log(self.added, test_kind, test, tag)
            else:
                self.removed.setdefault(test_kind, {}).setdefault(test, {})[tag] = failure_rate

    def clean_up_tag(self, test_kind, test, tag):
        """Clean up an invalid tag."""
        self.lifecycle.remove_tag(test_kind, test, tag)
        self.cleaned_up.setdefault(test_kind, {}).setdefault(test, []).append(tag)

    def clean_up_test(self, test_kind, test):
        """Clean up an invalid test."""
        self.lifecycle.remove_test_pattern(test_kind, test)
        self.cleaned_up.setdefault(test_kind, {})[test] = []


class JiraIssueCreator(object):
    """JiraIssueCreator class."""

    _LABEL = "test-lifecycle"
    _PROJECT = "TIGBOT"
    _MAX_DESCRIPTION_SIZE = 32767

    def __init__(  # pylint: disable=too-many-arguments
            self, server=None, username=None, password=None, access_token=None,
            access_token_secret=None, consumer_key=None, key_cert=None):
        """Initialize JiraIssueCreator."""
        self._client = jiraclient.JiraClient(
            server=server, username=username, password=password, access_token=access_token,
            access_token_secret=access_token_secret, consumer_key=consumer_key, key_cert=key_cert)

    def create_issue(  # pylint: disable=too-many-arguments
            self, evg_project, mongo_revision, model_config, added, removed, cleaned_up):
        """Create a JIRA issue for the test lifecycle tag update."""
        summary = self._get_jira_summary(evg_project)
        description = self._get_jira_description(evg_project, mongo_revision, model_config, added,
                                                 removed, cleaned_up)
        issue_key = self._client.create_issue(self._PROJECT, summary, description, [self._LABEL])
        return issue_key

    def close_fix_issue(self, issue_key):
        """Close the issue with the "Fixed" resolution."""
        LOGGER.info("Closing issue '%s' as FIXED.", issue_key)
        self._client.close_issue(issue_key, self._client.FIXED_RESOLUTION_NAME)

    def close_wontfix_issue(self, issue_key):
        """Close the issue the with "Won't Fix" resolution."""
        LOGGER.info("Closing issue '%s' as WON'T FIX.", issue_key)
        self._client.close_issue(issue_key, self._client.WONT_FIX_RESOLUTION_NAME)

    @staticmethod
    def _get_jira_summary(project):
        return "Update of test lifecycle tags for {}".format(project)

    @staticmethod
    def _monospace(text):
        """Transform a text into a monospace JIRA text."""
        return "{{" + text + "}}"

    @staticmethod
    def _truncate_description(desc):
        max_size = JiraIssueCreator._MAX_DESCRIPTION_SIZE
        if len(desc) > max_size:
            warning = ("\nDescription truncated: "
                       "exceeded max size of {} characters.").format(max_size)
            truncated_length = max_size - len(warning)
            desc = desc[:truncated_length] + warning
        return desc

    @staticmethod
    def _get_jira_description(  # pylint: disable=too-many-arguments
            project, mongo_revision, model_config, added, removed, cleaned_up):
        mono = JiraIssueCreator._monospace
        config_desc = _config_as_options(model_config)
        added_desc = JiraIssueCreator._make_updated_tags_description(added)
        removed_desc = JiraIssueCreator._make_updated_tags_description(removed)
        cleaned_up_desc = JiraIssueCreator._make_tags_cleaned_up_description(cleaned_up)
        project_link = "[{0}|https://evergreen.mongodb.com/waterfall/{1}]".format(
            mono(project), project)
        revision_link = "[{0}|https://github.com/mongodb/mongo/commit/{1}]".format(
            mono(mongo_revision), mongo_revision)
        full_desc = ("h3. Automatic update of the test lifecycle tags\n"
                     "Evergreen Project: {0}\n"
                     "Revision: {1}\n\n"
                     "{{{{update_test_lifecycle.py}}}} options:\n{2}\n\n"
                     "h5. Tags added\n{3}\n\n"
                     "h5. Tags removed\n{4}\n\n"
                     "h5. Tags cleaned up (no longer relevant)\n{5}\n").format(
                         project_link, revision_link, mono(config_desc), added_desc, removed_desc,
                         cleaned_up_desc)

        return JiraIssueCreator._truncate_description(full_desc)

    @staticmethod
    def _make_updated_tags_description(data):
        mono = JiraIssueCreator._monospace
        tags_lines = []
        for test_kind in sorted(data.keys()):
            tests = data[test_kind]
            tags_lines.append("- *{0}*".format(test_kind))
            for test in sorted(tests.keys()):
                tags = tests[test]
                tags_lines.append("-- {0}".format(mono(test)))
                for tag in sorted(tags.keys()):
                    coefficient = tags[tag]
                    tags_lines.append("--- {0} ({1:.2f})".format(mono(tag), coefficient))
        if tags_lines:
            return "\n".join(tags_lines)
        return "_None_"

    @staticmethod
    def _make_tags_cleaned_up_description(cleaned_up):
        mono = JiraIssueCreator._monospace
        tags_cleaned_up_lines = []
        for test_kind in sorted(cleaned_up.keys()):
            test_tags = cleaned_up[test_kind]
            tags_cleaned_up_lines.append("- *{0}*".format(test_kind))
            for test in sorted(test_tags.keys()):
                tags = test_tags[test]
                tags_cleaned_up_lines.append("-- {0}".format(mono(test)))
                if not tags:
                    tags_cleaned_up_lines.append("--- ALL (test file removed or renamed as part of"
                                                 " an earlier commit)")
                else:
                    for tag in sorted(tags):
                        tags_cleaned_up_lines.append("--- {0}".format(mono(tag)))
        if tags_cleaned_up_lines:
            return "\n".join(tags_cleaned_up_lines)
        return "_None_"


class LifecycleTagsFile(object):  # pylint: disable=too-many-instance-attributes
    """Represent a test lifecycle tags file that can be written and committed."""

    def __init__(  # pylint: disable=too-many-arguments
            self, project, lifecycle_file, metadata_repo_url=None, references_file=None,
            jira_issue_creator=None, git_info=None,
            model_config=None):  # noqa: D214,D401,D405,D406,D407,D411,D413
        """Initalize the LifecycleTagsFile.

        Arguments:
            project: The Evergreen project name, e.g. "mongodb-mongo-master".
            lifecycle_file: The path to the lifecycle tags file. If 'metadata_repo_url' is
                specified, this path must be relative to the root of the metadata repository.
            metadata_repo_url: The URL of the metadat repository that contains the test lifecycle
                tags file.
            references_file: The path to the references file in the metadata repository.
            jira_issue_creator: A JiraIssueCreator instance.
            git_info: A tuple containing the git user's name and email to set before committing.
            model_config: The model configuration as a Config instance.
        """
        self.project = project
        self.mongo_repo = git.Repository(os.getcwd())
        self.mongo_revision = self.mongo_repo.get_current_revision()
        # The branch name is the same on both repositories.
        self.mongo_branch = self.mongo_repo.get_branch_name()
        self.metadata_branch = project

        if metadata_repo_url:
            # The file can be found in another repository. We clone it.
            self.metadata_repo = self._clone_repository(metadata_repo_url, self.project)
            self.relative_lifecycle_file = lifecycle_file
            self.lifecycle_file = os.path.join(self.metadata_repo.directory, lifecycle_file)
            self.relative_references_file = references_file
            self.references_file = os.path.join(self.metadata_repo.directory, references_file)
            if git_info:
                self.metadata_repo.configure("user.name", git_info[0])
                self.metadata_repo.configure("user.email", git_info[1])
        else:
            self.metadata_repo = None
            self.relative_lifecycle_file = lifecycle_file
            self.lifecycle_file = lifecycle_file
            self.relative_references_file = None
            self.references_file = None
        self.metadata_repo_url = metadata_repo_url
        self.lifecycle = ci_tags.TagsConfig.from_file(self.lifecycle_file, cmp_func=compare_tags)
        self.jira_issue_creator = jira_issue_creator
        self.model_config = model_config
        self.changelog_lifecycle = TagsConfigWithChangelog(self.lifecycle)

    @staticmethod
    def _clone_repository(metadata_repo_url, branch):
        directory_name = posixpath.splitext(posixpath.basename(metadata_repo_url))[0]
        LOGGER.info("Cloning the repository %s into the directory %s", metadata_repo_url,
                    directory_name)
        return git.Repository.clone(metadata_repo_url, directory_name, branch)

    def is_modified(self):
        """Indicate if the tags have been modified."""
        return self.lifecycle.is_modified()

    def _create_issue(self):
        LOGGER.info("Creating a JIRA issue")
        issue_key = self.jira_issue_creator.create_issue(
            self.project, self.mongo_revision, self.model_config, self.changelog_lifecycle.added,
            self.changelog_lifecycle.removed, self.changelog_lifecycle.cleaned_up)
        LOGGER.info("JIRA issue created: %s", issue_key)
        return issue_key

    def write(self):
        """Write the test lifecycle tag file."""
        LOGGER.info("Writing the tag file to '%s'", self.lifecycle_file)
        comment = ("This file was generated by {} and shouldn't be edited by hand. It was"
                   " generated against commit {} with the following options: {}.").format(
                       sys.argv[0], self.mongo_repo.get_current_revision(),
                       _config_as_options(self.model_config))
        self.lifecycle.write_file(self.lifecycle_file, comment)

    def _ready_for_commit(self, ref_branch, references):
        # Check that the test lifecycle tags file has changed.
        diff = self.metadata_repo.git_diff(
            ["--name-only", ref_branch, self.relative_lifecycle_file])
        if not diff:
            LOGGER.info("The local lifecycle file is identical to the the one on branch '%s'",
                        ref_branch)
            return False
        # Check that the lifecycle file has not been updated after the current mongo revision.
        update_revision = references.get("test-lifecycle", {}).get(self.project)
        if update_revision and not self.mongo_repo.is_ancestor(update_revision,
                                                               self.mongo_revision):
            LOGGER.warning(("The existing lifecycle file is based on revision '%s' which is not a"
                            " parent revision of the current revision '%s'"), update_revision,
                           self.mongo_revision)
            return False
        return True

    def _read_references(self, metadata_branch=None):
        branch = metadata_branch if metadata_branch is not None else ""
        references_content = self.metadata_repo.git_cat_file(
            ["blob", "{0}:{1}".format(branch, self.relative_references_file)])
        return yaml.safe_load(references_content)

    def _update_and_write_references(self, references):
        LOGGER.info("Writing the references file to '%s'", self.references_file)
        references.setdefault("test-lifecycle", {})[self.project] = self.mongo_revision
        with open(self.references_file, "w") as fstream:
            yaml.safe_dump(references, fstream, default_flow_style=False)

    def _commit_locally(self, issue_key):
        self.metadata_repo.git_add([self.relative_lifecycle_file])
        self.metadata_repo.git_add([self.relative_references_file])
        commit_message = "{} Update {}".format(issue_key, self.relative_lifecycle_file)
        self.metadata_repo.commit_with_message(commit_message)
        LOGGER.info("Change committed with message: %s", commit_message)

    def commit(self, nb_retries=10):
        """Commit the test lifecycle tag file.

        Args:
            nb_retries: the number of times the script will reset, fetch, recommit and retry when
                the push fails.
        """
        references = self._read_references()
        # Verify we are ready to commit.
        if not self._ready_for_commit(self.metadata_branch, references):
            return True

        # Write the references file.
        self._update_and_write_references(references)

        # Create the issue.
        issue_key = self._create_issue()

        # Commit the change.
        self._commit_locally(issue_key)

        # Push the change.
        tries = 0
        pushed = False
        upstream = "origin/{0}".format(self.metadata_branch)
        while tries < nb_retries:
            try:
                self.metadata_repo.push_to_remote_branch("origin", self.metadata_branch)
                pushed = True
                break
            except git.GitException:
                LOGGER.warning("git push command failed, fetching and retrying.")
                # Fetch upstream branch.
                LOGGER.info("Fetching branch %s of %s", self.metadata_branch,
                            self.metadata_repo_url)
                self.metadata_repo.fetch_remote_branch("origin", self.metadata_branch)
                # Resetting the current branch to the origin branch
                LOGGER.info("Resetting branch %s to %s", self.metadata_branch, upstream)
                self.metadata_repo.git_reset(["--hard", upstream])
                # Rewrite the test lifecycle tags file
                self.write()
                # Rewrite the references file
                references = self._read_references()
                self._update_and_write_references(references)
                # Checking if we can still commit
                if not self._ready_for_commit(upstream, references):
                    LOGGER.warning("Aborting.")
                    break
                # Committing
                self._commit_locally(issue_key)
            tries += 1
        if pushed:
            self.jira_issue_creator.close_fix_issue(issue_key)
            return True
        self.jira_issue_creator.close_wontfix_issue(issue_key)
        return False


def make_lifecycle_tags_file(options, model_config):
    """Create a LifecycleTagsFile based on the script options."""
    if options.commit:
        if not options.jira_config:
            LOGGER.error("JIRA configuration file is required when specifying --commit.")
            return None
        if not (options.git_user_name or options.git_user_email):
            LOGGER.error("Git configuration parameters are required when specifying --commit.")
            return None
        jira_issue_creator = JiraIssueCreator(**utils.load_yaml_file(options.jira_config))
        git_config = (options.git_user_name, options.git_user_email)
    else:
        jira_issue_creator = None
        git_config = None

    lifecycle_tags_file = LifecycleTagsFile(options.project, options.tag_file,
                                            options.metadata_repo_url, options.references_file,
                                            jira_issue_creator, git_config, model_config)

    return lifecycle_tags_file


def main():  # pylint: disable=too-many-branches,too-many-locals,too-many-statements
    """Exexcute utility to update a resmoke.py tag file.

    This is based on computing test failure rates from the Evergreen API.
    """

    parser = optparse.OptionParser(
        description=textwrap.dedent(main.__doc__), usage="Usage: %prog [options] [test1 test2 ...]")

    data_options = optparse.OptionGroup(
        parser, title="Data options",
        description=("Options used to configure what historical test failure data to retrieve from"
                     " Evergreen."))
    parser.add_option_group(data_options)

    data_options.add_option("--project", dest="project", metavar="<project-name>",
                            default=tf.TestHistory.DEFAULT_PROJECT,
                            help="The Evergreen project to analyze. Defaults to '%default'.")

    data_options.add_option(
        "--tasks", dest="tasks", metavar="<task1,task2,...>",
        help=("The Evergreen tasks to analyze for tagging unreliable tests. If specified in"
              " additional to having test positional arguments, then only tests that run under the"
              " specified Evergreen tasks will be analyzed. If omitted, then the list of tasks"
              " defaults to the non-excluded list of tasks from the specified"
              " --evergreenProjectConfig file."))

    data_options.add_option(
        "--variants", dest="variants", metavar="<variant1,variant2,...>", default="",
        help="The Evergreen build variants to analyze for tagging unreliable tests.")

    data_options.add_option("--distros", dest="distros", metavar="<distro1,distro2,...>",
                            default="",
                            help="The Evergreen distros to analyze for tagging unreliable tests.")

    data_options.add_option(
        "--evergreenProjectConfig", dest="evergreen_project_config",
        metavar="<project-config-file>", default="etc/evergreen.yml",
        help=("The Evergreen project configuration file used to get the list of tasks if --tasks is"
              " omitted. Defaults to '%default'."))

    model_options = optparse.OptionGroup(
        parser, title="Model options",
        description=("Options used to configure whether (test,), (test, task),"
                     " (test, task, variant), and (test, task, variant, distro) combinations are"
                     " considered unreliable."))
    parser.add_option_group(model_options)

    model_options.add_option(
        "--reliableTestMinRuns", type="int", dest="reliable_test_min_runs",
        metavar="<reliable-min-runs>", default=DEFAULT_CONFIG.reliable_min_runs,
        help=("The minimum number of test executions required for a test's failure rate to"
              " determine whether the test is considered reliable. If a test has fewer than"
              " <reliable-min-runs> executions, then it cannot be considered unreliable."))

    model_options.add_option(
        "--unreliableTestMinRuns", type="int", dest="unreliable_test_min_runs",
        metavar="<unreliable-min-runs>", default=DEFAULT_CONFIG.unreliable_min_runs,
        help=("The minimum number of test executions required for a test's failure rate to"
              " determine whether the test is considered unreliable. If a test has fewer than"
              " <unreliable-min-runs> executions, then it cannot be considered unreliable."))

    model_options.add_option(
        "--testFailRates", type="float", nargs=2, dest="test_fail_rates",
        metavar="<test-acceptable-fail-rate> <test-unacceptable-fail-rate>",
        default=DEFAULT_CONFIG.test_fail_rates,
        help=("Controls how readily a test is considered unreliable. Each failure rate must be a"
              " number between 0 and 1 (inclusive) with"
              " <test-unacceptable-fail-rate> >= <test-acceptable-fail-rate>. If a test fails no"
              " more than <test-acceptable-fail-rate> in <reliable-days> time, then it is"
              " considered reliable. Otherwise, if a test fails at least as much as"
              " <test-unacceptable-fail-rate> in <test-unreliable-days> time, then it is considered"
              " unreliable. Defaults to %default."))

    model_options.add_option(
        "--taskFailRates", type="float", nargs=2, dest="task_fail_rates",
        metavar="<task-acceptable-fail-rate> <task-unacceptable-fail-rate>",
        default=DEFAULT_CONFIG.task_fail_rates,
        help=("Controls how readily a (test, task) combination is considered unreliable. Each"
              " failure rate must be a number between 0 and 1 (inclusive) with"
              " <task-unacceptable-fail-rate> >= <task-acceptable-fail-rate>. If a (test, task)"
              " combination fails no more than <task-acceptable-fail-rate> in <reliable-days> time,"
              " then it is considered reliable. Otherwise, if a test fails at least as much as"
              " <task-unacceptable-fail-rate> in <unreliable-days> time, then it is considered"
              " unreliable. Defaults to %default."))

    model_options.add_option(
        "--variantFailRates", type="float", nargs=2, dest="variant_fail_rates",
        metavar="<variant-acceptable-fail-rate> <variant-unacceptable-fail-rate>",
        default=DEFAULT_CONFIG.variant_fail_rates,
        help=("Controls how readily a (test, task, variant) combination is considered unreliable."
              " Each failure rate must be a number between 0 and 1 (inclusive) with"
              " <variant-unacceptable-fail-rate> >= <variant-acceptable-fail-rate>. If a"
              " (test, task, variant) combination fails no more than <variant-acceptable-fail-rate>"
              " in <reliable-days> time, then it is considered reliable. Otherwise, if a test fails"
              " at least as much as <variant-unacceptable-fail-rate> in <unreliable-days> time,"
              " then it is considered unreliable. Defaults to %default."))

    model_options.add_option(
        "--distroFailRates", type="float", nargs=2, dest="distro_fail_rates",
        metavar="<distro-acceptable-fail-rate> <distro-unacceptable-fail-rate>",
        default=DEFAULT_CONFIG.distro_fail_rates,
        help=("Controls how readily a (test, task, variant, distro) combination is considered"
              " unreliable. Each failure rate must be a number between 0 and 1 (inclusive) with"
              " <distro-unacceptable-fail-rate> >= <distro-acceptable-fail-rate>. If a"
              " (test, task, variant, distro) combination fails no more than"
              " <distro-acceptable-fail-rate> in <reliable-days> time, then it is considered"
              " reliable. Otherwise, if a test fails at least as much as"
              " <distro-unacceptable-fail-rate> in <unreliable-days> time, then it is considered"
              " unreliable. Defaults to %default."))

    model_options.add_option(
        "--reliableDays", type="int", dest="reliable_days", metavar="<ndays>",
        default=DEFAULT_CONFIG.reliable_time_period.days,
        help=("The time period to analyze when determining if a test has become reliable. Defaults"
              " to %default day(s)."))

    model_options.add_option(
        "--unreliableDays", type="int", dest="unreliable_days", metavar="<ndays>",
        default=DEFAULT_CONFIG.unreliable_time_period.days,
        help=("The time period to analyze when determining if a test has become unreliable."
              " Defaults to %default day(s)."))

    parser.add_option("--resmokeTagFile", dest="tag_file", metavar="<tagfile>",
                      default="etc/test_lifecycle.yml",
                      help=("The resmoke.py tag file to update. If --metadataRepo is specified, it"
                            " is the relative path in the metadata repository, otherwise it can be"
                            " an absolute path or a relative path from the current directory."
                            " Defaults to '%default'."))

    parser.add_option("--metadataRepo", dest="metadata_repo_url", metavar="<metadata-repo-url>",
                      default="git@github.com:mongodb/mongo-test-metadata.git",
                      help=("The repository that contains the lifecycle file. "
                            "It will be cloned in the current working directory. "
                            "Defaults to '%default'."))

    parser.add_option("--referencesFile", dest="references_file", metavar="<references-file>",
                      default="references.yml",
                      help=("The YAML file in the metadata repository that contains the revision "
                            "mappings. Defaults to '%default'."))

    parser.add_option("--requestBatchSize", type="int", dest="batch_size", metavar="<batch-size>",
                      default=100,
                      help=("The maximum number of tests to query the Evergreen API for in a single"
                            " request. A higher value for this option will reduce the number of"
                            " roundtrips between this client and Evergreen. Defaults to %default."))

    parser.add_option("--requestThreads", type="int", dest="num_request_threads",
                      metavar="<num-request-threads>", default=DEFAULT_NUM_THREADS,
                      help=("The maximum number of threads to use when querying the Evergreen API."
                            " Batches are processed sequentially but the test history is queried in"
                            " parallel for each task. Defaults to %default."))

    commit_options = optparse.OptionGroup(
        parser, title="Commit options",
        description=("Options used to configure whether and how to commit the updated test"
                     " lifecycle tags."))
    parser.add_option_group(commit_options)

    commit_options.add_option("--commit", action="store_true", dest="commit", default=False,
                              help="Indicates that the updated tag file should be committed.")

    commit_options.add_option(
        "--jiraConfig", dest="jira_config", metavar="<jira-config>", default=None,
        help=("The YAML file containing the JIRA access configuration ('user', 'password',"
              "'server')."))

    commit_options.add_option(
        "--gitUserName", dest="git_user_name", metavar="<git-user-name>", default="Test Lifecycle",
        help=("The git user name that will be set before committing to the metadata repository."
              " Defaults to '%default'."))

    commit_options.add_option(
        "--gitUserEmail", dest="git_user_email", metavar="<git-user-email>",
        default="buil+testlifecycle@mongodb.com",
        help=("The git user email address that will be set before committing to the metadata"
              " repository. Defaults to '%default'."))

    logging_options = optparse.OptionGroup(
        parser, title="Logging options",
        description="Options used to configure the logging output of the script.")
    parser.add_option_group(logging_options)

    logging_options.add_option("--logLevel", dest="log_level", metavar="<log-level>", choices=[
        "DEBUG", "INFO", "WARNING", "ERROR"
    ], default="INFO", help=("The log level. Accepted values are: DEBUG, INFO, WARNING and ERROR."
                             " Defaults to '%default'."))

    logging_options.add_option(
        "--logFile", dest="log_file", metavar="<log-file>", default=None,
        help="The destination file for the logs output. Defaults to the standard output.")

    (options, tests) = parser.parse_args()

    if options.distros:
        warnings.warn(
            ("Until https://jira.mongodb.org/browse/EVG-1665 is implemented, distro information"
             " isn't returned by the Evergreen API. This option will therefore be ignored."),
            RuntimeWarning)

    logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s", level=options.log_level,
                        filename=options.log_file)
    evg_conf = ci_evergreen.parse_evergreen_file(options.evergreen_project_config)
    use_test_tasks_membership = False

    tasks = options.tasks.split(",") if options.tasks else []
    if not tasks:
        # If no tasks are specified, then the list of tasks is all.
        tasks = evg_conf.lifecycle_task_names
        use_test_tasks_membership = True

    variants = options.variants.split(",") if options.variants else []

    distros = options.distros.split(",") if options.distros else []

    config = Config(
        test_fail_rates=Rates(*options.test_fail_rates),
        task_fail_rates=Rates(*options.task_fail_rates),
        variant_fail_rates=Rates(*options.variant_fail_rates),
        distro_fail_rates=Rates(*options.distro_fail_rates),
        reliable_min_runs=options.reliable_test_min_runs,
        reliable_time_period=datetime.timedelta(days=options.reliable_days),
        unreliable_min_runs=options.unreliable_test_min_runs,
        unreliable_time_period=datetime.timedelta(days=options.unreliable_days))
    validate_config(config)

    lifecycle_tags_file = make_lifecycle_tags_file(options, config)
    if not lifecycle_tags_file:
        sys.exit(1)

    test_tasks_membership = get_test_tasks_membership(evg_conf)
    # If no tests are specified then the list of tests is generated from the list of tasks.
    if not tests:
        tests = get_tests_from_tasks(tasks, test_tasks_membership)
        if not options.tasks:
            use_test_tasks_membership = True

    commit_first, commit_last = git_commit_range_since("{}.days".format(options.unreliable_days))
    commit_prior = git_commit_prior(commit_first)

    # For efficiency purposes, group the tests and process in batches of batch_size.
    test_groups = create_batch_groups(create_test_groups(tests), options.batch_size)

    test_history_source = TestHistorySource(options.project, variants, distros, commit_prior,
                                            commit_last, options.num_request_threads)

    LOGGER.info("Updating the tags")
    nb_groups = len(test_groups)
    count = 0
    for tests in test_groups:
        LOGGER.info("Progress: %s %%", 100 * count / nb_groups)
        count += 1
        # Find all associated tasks for the test_group if tasks or tests were not specified.
        if use_test_tasks_membership:
            tasks_set = set()
            for test in tests:
                tasks_set = tasks_set.union(test_tasks_membership[test])
            tasks = list(tasks_set)
        if not tasks:
            LOGGER.warning("No tasks found for tests %s, skipping this group.", tests)
            continue
        history_data = test_history_source.get_history_data(tests, tasks)
        if not history_data:
            continue
        report = tf.Report(history_data)
        update_tags(lifecycle_tags_file.changelog_lifecycle, config, report, tests)

    # Remove tags that are no longer relevant
    clean_up_tags(lifecycle_tags_file.changelog_lifecycle, evg_conf)

    # We write the 'lifecycle' tag configuration to the 'options.lifecycle_file' file only if there
    # have been changes to the tags. In particular, we avoid modifying the file when only the header
    # comment for the YAML file would change.
    if lifecycle_tags_file.is_modified():
        lifecycle_tags_file.write()

        if options.commit:
            commit_ok = lifecycle_tags_file.commit()
            if not commit_ok:
                sys.exit(1)
    else:
        LOGGER.info("The tags have not been modified.")


if __name__ == "__main__":
    main()
