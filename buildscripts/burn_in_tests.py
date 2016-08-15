#!/usr/bin/env python

"""
Command line utility for determining what jstests have been added or modified
"""

from __future__ import absolute_import

import collections
import json
import optparse
import os.path
import subprocess
import re
import sys
import yaml


# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from buildscripts import resmokelib


def parse_command_line():
    parser = optparse.OptionParser(usage="Usage: %prog [options] [resmoke command]")

    parser.add_option("--maxRevisions", dest="max_revisions",
                      help="Maximum number of revisions to check for changes. Default is 25.")

    parser.add_option("--branch", dest="branch",
                      help="The name of the branch the working branch was based on.")

    parser.add_option("--baseCommit", dest="base_commit",
                      help="The base commit to compare to for determining changes.")

    parser.add_option("--buildVariant", dest="buildvariant",
                      help="The buildvariant the tasks will execute on. \
                            Required when generating the JSON file with test executor information")

    parser.add_option("--noExec", dest="no_exec", action="store_true",
                      help="Do not run resmoke loop on new tests.")

    parser.add_option("--reportFile", dest="report_file",
                      help="Write a JSON file with test results.")

    parser.add_option("--testListFile", dest="test_list_file", metavar="TESTLIST",
                      help="Load a JSON file with tests to run.")

    parser.add_option("--testListOutfile", dest="test_list_outfile",
                      help="Write a JSON file with test executor information.")

    # The executor_file and suite_files defaults are required to make the
    # suite resolver work correctly.
    parser.set_defaults(base_commit=None,
                        branch="master",
                        buildvariant=None,
                        evergreen_file="etc/evergreen.yml",
                        executor_file="with_server",
                        max_revisions=25,
                        no_exec=False,
                        report_file="report.json",
                        suite_files=None,
                        test_list_file=None,
                        test_list_outfile=None)

    # This disables argument parsing on the first unrecognized parameter. This allows us to pass
    # a complete resmoke.py command line without accidentally parsing its options.
    parser.disable_interspersed_args()

    return parser.parse_args()


# Copied from python 2.7 version of subprocess.py
# Exception classes used by this module.
class CalledProcessError(Exception):
    """This exception is raised when a process run by check_call() or
    check_output() returns a non-zero exit status.
    The exit status will be stored in the returncode attribute;
    check_output() will also store the output in the output attribute.
    """
    def __init__(self, returncode, cmd, output=None):
        self.returncode = returncode
        self.cmd = cmd
        self.output = output
    def __str__(self):
        return ("Command '%s' returned non-zero exit status %d with output %s" %
                (self.cmd, self.returncode, self.output))


# Copied from python 2.7 version of subprocess.py
def check_output(*popenargs, **kwargs):
    """Run command with arguments and return its output as a byte string.

    If the exit code was non-zero it raises a CalledProcessError.  The
    CalledProcessError object will have the return code in the returncode
    attribute and output in the output attribute.

    The arguments are the same as for the Popen constructor.  Example:

    >>> check_output(["ls", "-l", "/dev/null"])
    'crw-rw-rw- 1 root root 1, 3 Oct 18  2007 /dev/null\n'

    The stdout argument is not allowed as it is used internally.
    To capture standard error in the result, use stderr=STDOUT.

    >>> check_output(["/bin/sh", "-c",
    ...               "ls -l non_existent_file ; exit 0"],
    ..              stderr=STDOUT)
    'ls: non_existent_file: No such file or directory\n'
    """
    if 'stdout' in kwargs:
        raise ValueError('stdout argument not allowed, it will be overridden.')
    process = subprocess.Popen(stdout=subprocess.PIPE, *popenargs, **kwargs)
    output, unused_err = process.communicate()
    retcode = process.poll()
    if retcode:
        cmd = kwargs.get("args")
        if cmd is None:
            cmd = popenargs[0]
        raise CalledProcessError(retcode, cmd, output)
    return output


def callo(args):
    """Call a program, and capture its output
    """
    return check_output(args)


def find_changed_tests(branch_name, base_commit, max_revisions):
    """
    Use git to find which files have changed in this patch.
    TODO: This should be expanded to search for enterprise modules.
    """

    changed_tests = []
    if base_commit is None:
        base_commit = callo(["git", "merge-base", branch_name + "@{upstream}", "HEAD"]).rstrip()
    revisions = callo(["git", "rev-list", base_commit + "...HEAD"]).splitlines()
    revision_count = len(revisions)

    if revision_count > max_revisions:
        print "There are too many revisions included (%d)." % revision_count, \
              "This is likely because your base branch is not " + branch_name + ".", \
              "You can allow us to review more than 25 revisions by using", \
              "the --maxRevisions option."
        return changed_tests

    changed_files = callo(["git", "diff", "--name-only", base_commit]).splitlines()
    # New files ("untracked" in git terminology) won't show up in the git diff results.
    untracked_files = callo(["git", "status", "--porcelain"]).splitlines()

    # The lines with untracked files start with '?? '.
    for line in untracked_files:
        if line.startswith("?"):
            (status, line) = line.split(" ")
            changed_files.append(line)

    for line in changed_files:
        line = line.rstrip()
        # Check that the file exists because it may have been moved or deleted in the patch.
        if os.path.splitext(line)[1] != ".js" or not os.path.isfile(line):
            continue
        if "jstests" in line:
            changed_tests.append(line)
    return changed_tests


def find_tests_by_executor(suites):
    """
    Looks up what other resmoke suites run the tests specified in the suites
    parameter. Returns a dict keyed by test name, value is array of suite names.
    """

    memberships = {}
    test_membership = resmokelib.parser.create_test_membership_map()
    for suite in suites:
        for group in suite.test_groups:
            for test in group.tests:
                memberships[test] = test_membership[test]
    return memberships


def create_executor_list(suites):
    """
    Looks up what other resmoke suites run the tests specified in the suites
    parameter. Returns a dict keyed by suite name / executor, value is tests
    to run under that executor.
    """

    memberships = collections.defaultdict(list)
    test_membership = resmokelib.parser.create_test_membership_map()
    for suite in suites:
        for group in suite.test_groups:
            for test in group.tests:
                for executor in test_membership[test]:
                    memberships[executor].append(test)
    return memberships


def create_buildvariant_list(evergreen_file):
    """
    Parses etc/evergreen.yml. Returns a list of buildvariants.
    """

    with open(evergreen_file, "r") as f:
        evg = yaml.load(f)

    return [li["name"] for li in evg["buildvariants"]]


def create_task_list(evergreen_file, buildvariant, suites):
    """
    Parses etc/evergreen.yml to find associated tasks for the specified buildvariant
    and suites. Returns a dict keyed by task_name, with executor, resmoke_args & tests, i.e.,
    {'jsCore_small_oplog':
        {'resmoke_args': '--suites=core_small_oplog --storageEngine=mmapv1',
         'tests': ['jstests/core/all2.js', 'jstests/core/all3.js']}
    }
    """

    # Check if buildvariant is in the evergreen_file.
    buildvariants = create_buildvariant_list(evergreen_file)
    if buildvariant not in buildvariants:
        print "Buildvariant", buildvariant, "not found in", evergreen_file
        sys.exit(1)

    with open(evergreen_file, "r") as f:
        evg = yaml.load(f)

    # Find all the task names for the specified buildvariant.
    variant_tasks = [li["name"] for li in next(item for item in evg["buildvariants"]
                                               if item["name"] == buildvariant)["tasks"]]

    # Find all the buildvariant task's resmoke_args.
    variant_task_args = {}
    for task in [a for a in evg["tasks"] if a["name"] in variant_tasks]:
        for command in task["commands"]:
            if ("func" in command and command["func"] == "run tests" and
                "vars" in command and "resmoke_args" in command["vars"]):
                variant_task_args[task["name"]] = command["vars"]["resmoke_args"]

    # Create the list of tasks to run for the specified suite.
    tasks_to_run = {}
    for suite in suites.keys():
        for task_name, task_arg in variant_task_args.items():
            # Find the resmoke_args for matching suite names.
            # Change the --suites to --executor
            if (re.compile('--suites=' + suite + '(?:\s+|$)').match(task_arg) or
                re.compile('--executor=' + suite + '(?:\s+|$)').match(task_arg)):
                tasks_to_run[task_name] = {
                    "resmoke_args": task_arg.replace("--suites", "--executor"),
                    "tests": suites[suite]}

    return tasks_to_run


def _write_report_file(tests_by_executor, pathname):
    """
    Writes out a JSON file containing the tests_by_executor dict.  This should
    be done during the compile task when the git repo is available.
    """
    with open(pathname, "w") as fp:
        json.dump(tests_by_executor, fp)


def _load_tests_file(pathname):
    """
    Load the list of tests and executors from the specified file. The file might
    not exist, and this is fine. The task running this becomes a nop.
    """
    if not os.path.isfile(pathname):
        return None
    with open(pathname, "r") as fp:
        return json.load(fp)


def _save_report_data(saved_data, pathname):
    """
    Read in the report file from the previous resmoke.py run if it exists. We'll concat it to the
    passed saved_data dict.
    """
    if not os.path.isfile(pathname):
        return None
    with open(pathname, "r") as fp:
        current_data = json.load(fp)
    saved_data["failures"] += current_data["failures"]
    saved_data["results"] += current_data["results"]


def main():
    values = collections.defaultdict(list)
    values, args = parse_command_line()

    # If a resmoke.py command wasn't passed in, use a simple version.
    if not args:
        args = ["python", "buildscripts/resmoke.py", "--repeat=2"]

    # Load the dict of tests to run.
    if values.test_list_file:
        tests_by_task = _load_tests_file(values.test_list_file)
        # If there are no tests to run, carry on.
        if tests_by_task is None:
            sys.exit(0)

    # Run the executor finder.
    else:
        if values.buildvariant is None:
            print "Option buildVariant must be specified to find changed tests.\n", \
                  "Select from the following: \n" \
                  "\t", "\n\t".join(sorted(create_buildvariant_list(values.evergreen_file)))
            sys.exit(1)
        changed_tests = find_changed_tests(values.branch, values.base_commit, values.max_revisions)
        # If there are no changed tests, exit cleanly.
        if not changed_tests:
            print "No new or modified tests found."
            sys.exit(0)
        suites = resmokelib.parser.get_suites(values, changed_tests)
        tests_by_executor = create_executor_list(suites)
        tests_by_task = create_task_list(values.evergreen_file, values.buildvariant, tests_by_executor)
        if values.test_list_outfile is not None:
            _write_report_file(tests_by_task, values.test_list_outfile)

    # If we're not in noExec mode, run the tests.
    if not values.no_exec:
        test_results = {"failures": 0, "results": []}

        for task in sorted(tests_by_task):
            try:
                subprocess.check_call(" ".join(args) + " " +
                                      tests_by_task[task]["resmoke_args"] + " " +
                                      " ".join(tests_by_task[task]["tests"]), shell=True)
            except subprocess.CalledProcessError as err:
                print "Resmoke returned an error with task:", task
                _save_report_data(test_results, values.report_file)
                _write_report_file(test_results, values.report_file)
                sys.exit(err.returncode)

            _save_report_data(test_results, values.report_file)
        _write_report_file(test_results, values.report_file)

    sys.exit(0)

if __name__ == "__main__":
    main()
