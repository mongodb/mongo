#!/usr/bin/env python
"""Command line utility for determining what jstests have been added or modified."""

from __future__ import absolute_import
from __future__ import print_function

import collections
import copy
import json
import optparse
import os.path
import subprocess
import re
import shlex
import sys
import urlparse

import requests
import yaml

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts import git
from buildscripts import resmokelib
from buildscripts.ciconfig import evergreen
from buildscripts.client import evergreen as evergreen_client
# pylint: enable=wrong-import-position

API_REST_PREFIX = "/rest/v1/"
API_SERVER_DEFAULT = "https://evergreen.mongodb.com"
REPEAT_SUITES = 2
EVERGREEN_FILE = "etc/evergreen.yml"
# The executor_file and suite_files defaults are required to make the suite resolver work
# correctly.
SELECTOR_FILE = "etc/burn_in_tests.yml"
SUITE_FILES = ["with_server"]


def parse_command_line():
    """Parse command line options."""

    parser = optparse.OptionParser(usage="Usage: %prog [options] [resmoke command]")

    parser.add_option("--maxRevisions", dest="max_revisions", default=25,
                      help=("Maximum number of revisions to check for changes. Default is"
                            " %default."))

    parser.add_option("--branch", dest="branch", default="master",
                      help=("The name of the branch the working branch was based on. Default is"
                            " '%default'."))

    parser.add_option("--baseCommit", dest="base_commit", default=None,
                      help="The base commit to compare to for determining changes.")

    parser.add_option("--buildVariant", dest="buildvariant", default=None,
                      help=("The buildvariant the tasks will execute on. Required when"
                            " generating the JSON file with test executor information"))

    parser.add_option("--checkEvergreen", dest="check_evergreen", default=False,
                      action="store_true",
                      help=("Checks Evergreen for the last commit that was scheduled."
                            " This way all the tests that haven't been burned in will be run."))

    parser.add_option("--noExec", dest="no_exec", default=False, action="store_true",
                      help="Do not run resmoke loop on new tests.")

    parser.add_option("--reportFile", dest="report_file", default="report.json",
                      help="Write a JSON file with test results. Default is '%default'.")

    parser.add_option("--testListFile", dest="test_list_file", default=None, metavar="TESTLIST",
                      help="Load a JSON file with tests to run.")

    parser.add_option("--testListOutfile", dest="test_list_outfile", default=None,
                      help="Write a JSON file with test executor information.")

    parser.add_option("--repeatTests", dest="repeat_tests_num", default=None,
                      help="The number of times to repeat each test. If --repeatTestsSecs is not"
                      " specified then this will be set to {}.".format(REPEAT_SUITES))

    parser.add_option("--repeatTestsMin", dest="repeat_tests_min", default=None,
                      help="The minimum number of times to repeat each test when --repeatTestsSecs"
                      " is specified.")

    parser.add_option("--repeatTestsMax", dest="repeat_tests_max", default=None,
                      help="The maximum number of times to repeat each test when --repeatTestsSecs"
                      " is specified.")

    parser.add_option("--repeatTestsSecs", dest="repeat_tests_secs", default=None,
                      help="Time, in seconds, to repeat each test. Note that this option is"
                      " mutually exclusive with with --repeatTests.")

    # This disables argument parsing on the first unrecognized parameter. This allows us to pass
    # a complete resmoke.py command line without accidentally parsing its options.
    parser.disable_interspersed_args()

    values, args = parser.parse_args()
    validate_options(parser, values)

    return values, args


def validate_options(parser, values):
    """Validate command line options."""

    if values.repeat_tests_max:
        if values.repeat_tests_secs is None:
            parser.error("Must specify --repeatTestsSecs with --repeatTestsMax")

        if values.repeat_tests_min and values.repeat_tests_min > values.repeat_tests_max:
            parser.error("--repeatTestsSecsMin is greater than --repeatTestsMax")

    if values.repeat_tests_min and values.repeat_tests_secs is None:
        parser.error("Must specify --repeatTestsSecs with --repeatTestsMin")

    if values.repeat_tests_num and values.repeat_tests_secs:
        parser.error("Cannot specify --repeatTests and --repeatTestsSecs")

    if values.test_list_file is None and values.buildvariant is None:
        parser.error("Must specify --buildVariant to find changed tests")

    if values.buildvariant:
        evg_conf = evergreen.parse_evergreen_file(EVERGREEN_FILE)
        if not evg_conf.get_variant(values.buildvariant):
            parser.error("Buildvariant '{}' not found in {}, select from:\n\t{}".format(
                values.buildvariant, EVERGREEN_FILE, "\n\t".join(sorted(evg_conf.variant_names))))


def find_last_activated_task(revisions, variant, branch_name):
    """Get the git hash of the most recently activated build before this one."""

    project = "mongodb-mongo-" + branch_name
    build_prefix = "mongodb_mongo_" + branch_name + "_" + variant.replace('-', '_')

    evg_cfg = evergreen_client.read_evg_config()
    if evg_cfg is not None and "api_server_host" in evg_cfg:
        api_server = "{url.scheme}://{url.netloc}".format(
            url=urlparse.urlparse(evg_cfg["api_server_host"]))
    else:
        api_server = API_SERVER_DEFAULT

    api_prefix = api_server + API_REST_PREFIX

    for githash in revisions:
        url = "{}projects/{}/revisions/{}".format(api_prefix, project, githash)
        response = requests.get(url)
        revision_data = response.json()

        try:
            for build in revision_data["builds"]:
                if build.startswith(build_prefix):
                    url = "{}builds/{}".format(api_prefix, build)
                    build_resp = requests.get(url)
                    build_data = build_resp.json()
                    if build_data["activated"]:
                        return build_data["revision"]
        except:  # pylint: disable=bare-except
            # Sometimes build data is incomplete, as was the related build.
            pass

    return None


def find_changed_tests(  # pylint: disable=too-many-locals
        branch_name, base_commit, max_revisions, buildvariant, check_evergreen):
    """Find the changed tests.

    Use git to find which files have changed in this patch.
    TODO: This should be expanded to search for enterprise modules.
    The returned file paths are in normalized form (see os.path.normpath(path)).
    """

    changed_tests = []

    repo = git.Repository(".")

    if base_commit is None:
        base_commit = repo.get_merge_base([branch_name + "@{upstream}", "HEAD"])
    if check_evergreen:
        # We're going to check up to 200 commits in Evergreen for the last scheduled one.
        # The current commit will be activated in Evergreen; we use --skip to start at the
        # previous commit when trying to find the most recent preceding commit that has been
        # activated.
        revs_to_check = repo.git_rev_list([base_commit, "--max-count=200", "--skip=1"]).splitlines()
        last_activated = find_last_activated_task(revs_to_check, buildvariant, branch_name)
        if last_activated is None:
            # When the current commit is the first time 'buildvariant' has run, there won't be a
            # commit among 'revs_to_check' that's been activated in Evergreen. We handle this by
            # only considering tests changed in the current commit.
            last_activated = "HEAD"
        print("Comparing current branch against", last_activated)
        revisions = repo.git_rev_list([base_commit + "..." + last_activated]).splitlines()
        base_commit = last_activated
    else:
        revisions = repo.git_rev_list([base_commit + "...HEAD"]).splitlines()

    revision_count = len(revisions)
    if revision_count > max_revisions:
        print(("There are too many revisions included ({}). This is likely because your base"
               " branch is not {}. You can allow us to review more than {} revisions by using"
               " the --maxRevisions option.".format(revision_count, branch_name, max_revisions)))
        return changed_tests

    changed_files = repo.git_diff(["--name-only", base_commit]).splitlines()
    # New files ("untracked" in git terminology) won't show up in the git diff results.
    untracked_files = repo.git_status(["--porcelain"]).splitlines()

    # The lines with untracked files start with '?? '.
    for line in untracked_files:
        if line.startswith("?"):
            (_, line) = line.split(" ", 1)
            changed_files.append(line)

    for line in changed_files:
        line = line.rstrip()
        # Check that the file exists because it may have been moved or deleted in the patch.
        if os.path.splitext(line)[1] != ".js" or not os.path.isfile(line):
            continue
        if "jstests" in line:
            path = os.path.normpath(line)
            changed_tests.append(path)
    return changed_tests


def find_excludes(selector_file):
    """Parse etc/burn_in_tests.yml. Returns lists of excluded suites, tasks & tests."""

    if not selector_file:
        return ([], [], [])

    with open(selector_file, "r") as fstream:
        yml = yaml.load(fstream)

    try:
        js_test = yml['selector']['js_test']
    except KeyError:
        raise Exception(
            "The selector file " + selector_file + " is missing the 'selector.js_test' key")

    return (resmokelib.utils.default_if_none(js_test.get("exclude_suites"), []),
            resmokelib.utils.default_if_none(js_test.get("exclude_tasks"), []),
            resmokelib.utils.default_if_none(js_test.get("exclude_tests"), []))


def filter_tests(tests, exclude_tests):
    """Exclude tests which have been blacklisted.

    A test is in the tests list, i.e., ['jstests/core/a.js']
    The tests paths must be in normalized form (see os.path.normpath(path)).
    """

    if not exclude_tests or not tests:
        return tests

    # The exclude_tests can be specified using * and ** to specify directory and file patterns.
    excluded_globbed = set()
    for exclude_test_pattern in exclude_tests:
        excluded_globbed.update(resmokelib.utils.globstar.iglob(exclude_test_pattern))

    return set(tests) - excluded_globbed


def create_executor_list(suites, exclude_suites):
    """Create the executor list.

    Looks up what other resmoke suites run the tests specified in the suites
    parameter. Returns a dict keyed by suite name / executor, value is tests
    to run under that executor.
    """

    memberships = collections.defaultdict(list)
    test_membership = resmokelib.suitesconfig.create_test_membership_map()
    for suite in suites:
        for test in suite.tests:
            for executor in set(test_membership[test]) - set(exclude_suites):
                if test not in memberships[executor]:
                    memberships[executor].append(test)
    return memberships


def create_task_list(evergreen_conf, buildvariant, suites, exclude_tasks):
    """Find associated tasks for the specified buildvariant and suites.

    Returns a dict keyed by task_name, with executor, resmoke_args & tests, i.e.,
    {'jsCore_small_oplog':
        {'resmoke_args': '--suites=core_small_oplog --storageEngine=inMemory',
         'tests': ['jstests/core/all2.js', 'jstests/core/all3.js']}
    }
    """

    evg_buildvariant = evergreen_conf.get_variant(buildvariant)
    if not evg_buildvariant:
        print("Buildvariant '{}' not found".format(buildvariant))
        sys.exit(1)

    # Find all the buildvariant task's resmoke_args.
    variant_task_args = {}
    exclude_tasks_set = set(exclude_tasks)
    for task in evg_buildvariant.tasks:
        if task.name not in exclude_tasks_set:
            # Using 'task.combined_resmoke_args' to include the variant's test_flags and
            # allow the storage engine to be overridden.
            resmoke_args = task.combined_resmoke_args
            if resmoke_args:
                variant_task_args[task.name] = resmoke_args

    # Create the list of tasks to run for the specified suite.
    tasks_to_run = {}
    for suite in suites.keys():
        for task_name, task_arg in variant_task_args.items():
            # Find the resmoke_args for matching suite names.
            if re.compile('--suites=' + suite + r'(?:\s+|$)').match(task_arg):
                tasks_to_run[task_name] = {"resmoke_args": task_arg, "tests": suites[suite]}

    return tasks_to_run


def _write_json_file(json_data, pathname):
    """Write out a JSON file."""

    with open(pathname, "w") as fstream:
        json.dump(json_data, fstream)


def _load_tests_file(pathname):
    """Load the list of tests and executors from the specified file.

    The file might not exist, and this is fine. The task running this becomes a noop.
    """

    if not os.path.isfile(pathname):
        return None
    with open(pathname, "r") as fstream:
        return json.load(fstream)


def _update_report_data(data_to_update, pathname, task):
    """Read in the report file from the previous resmoke.py run, if it exists.

    We'll concat it to the data_to_update dict.
    """

    if not os.path.isfile(pathname):
        return

    with open(pathname, "r") as fstream:
        report_data = json.load(fstream)

    for result in report_data["results"]:
        result["test_file"] += ":" + task

    data_to_update["failures"] += report_data["failures"]
    data_to_update["results"] += report_data["results"]


def _set_resmoke_cmd(values, args):
    """Build the resmoke command, if a resmoke.py command wasn't passed in."""

    if values.repeat_tests_secs:
        repeat_options = "--repeatTestsSecs={}".format(values.repeat_tests_secs)
        if values.repeat_tests_min:
            repeat_options += " --repeatTestsMin={}".format(values.repeat_tests_min)
        if values.repeat_tests_max:
            repeat_options += " --repeatTestsMax={}".format(values.repeat_tests_max)
    else:
        # To maintain previous default behavior, we set repeat_suites to 2 if
        # values.repeat_tests_secs and values.repeat_tests_num are both not specified.
        repeat_suites = values.repeat_tests_num if values.repeat_tests_num else REPEAT_SUITES
        repeat_options = "--repeatSuites={}".format(repeat_suites)
    new_args = copy.deepcopy(args) if args else ["python", "buildscripts/resmoke.py"]
    new_args += repeat_options.split()

    return new_args


def run_tests(no_exec, tests_by_task, resmoke_cmd, report_file):
    """Run the tests if not in no_exec mode."""

    if no_exec:
        return

    test_results = {"failures": 0, "results": []}

    for task in sorted(tests_by_task):
        new_resmoke_cmd = copy.deepcopy(resmoke_cmd)
        new_resmoke_cmd.extend(shlex.split(tests_by_task[task]["resmoke_args"]))
        new_resmoke_cmd.extend(tests_by_task[task]["tests"])
        try:
            subprocess.check_call(new_resmoke_cmd, shell=False)
        except subprocess.CalledProcessError as err:
            print("Resmoke returned an error with task:", task)
            _update_report_data(test_results, report_file, task)
            _write_json_file(test_results, report_file)
            sys.exit(err.returncode)

        # Note - _update_report_data concatenates to test_results the current results to the
        # previously saved results.
        _update_report_data(test_results, report_file, task)

    _write_json_file(test_results, report_file)


def main():
    """Execute Main program."""

    values, args = parse_command_line()

    resmoke_cmd = _set_resmoke_cmd(values, args)

    # Load the dict of tests to run.
    if values.test_list_file:
        tests_by_task = _load_tests_file(values.test_list_file)
        # If there are no tests to run, carry on.
        if tests_by_task is None:
            test_results = {"failures": 0, "results": []}
            _write_json_file(test_results, values.report_file)
            sys.exit(0)

    # Run the executor finder.
    else:
        # Parse the Evergreen project configuration file.
        evergreen_conf = evergreen.parse_evergreen_file(EVERGREEN_FILE)

        changed_tests = find_changed_tests(values.branch, values.base_commit, values.max_revisions,
                                           values.buildvariant, values.check_evergreen)
        exclude_suites, exclude_tasks, exclude_tests = find_excludes(SELECTOR_FILE)
        changed_tests = filter_tests(changed_tests, exclude_tests)

        if changed_tests:
            suites = resmokelib.suitesconfig.get_suites(suite_files=SUITE_FILES,
                                                        test_files=changed_tests)
            tests_by_executor = create_executor_list(suites, exclude_suites)
            tests_by_task = create_task_list(evergreen_conf, values.buildvariant, tests_by_executor,
                                             exclude_tasks)
        else:
            print("No new or modified tests found.")
            tests_by_task = {}
        if values.test_list_outfile:
            _write_json_file(tests_by_task, values.test_list_outfile)

    run_tests(values.no_exec, tests_by_task, resmoke_cmd, values.report_file)


if __name__ == "__main__":
    main()
