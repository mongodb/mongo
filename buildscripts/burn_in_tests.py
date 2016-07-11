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
import sys

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

    parser.add_option("--noExec", dest="no_exec", action="store_true",
                      help="Do not run resmoke loop on new tests.")

    parser.add_option("--reportFile", dest="report_file",
                      help="Write a JSON file with test executor information.")

    parser.add_option("--skipEnterpriseSuites", dest="no_enterprise", action="store_true",
                      help="Do not run against enterprise specific executors.")

    parser.add_option("--testListFile", dest="tests_file", metavar="TESTLIST",
                      help="Load a JSON file with tests to run.")

    # The executor_file and suite_files defaults are required to make the
    # suite resolver work correctly.
    parser.set_defaults(base_commit=None,
                        branch="master",
                        executor_file="with_server",
                        max_revisions=25,
                        no_exec=False,
                        no_enterprise=False,
                        suite_files=None)

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

    changed_files = callo(["git", "diff", "--name-only", base_commit])
    for line in changed_files.splitlines():
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
    if values.tests_file:
        tests_by_executor = _load_tests_file(values.tests_file)
        # If there are no tests to run, carry on.
        if tests_by_executor is None:
            sys.exit(0)

    # Run the executor finder.
    else:
        changed_tests = find_changed_tests(values.branch, values.base_commit, values.max_revisions)
        # If there are no changed tests, exit cleanly.
        if not changed_tests:
            print "No new or modified tests found."
            sys.exit(0)
        suites = resmokelib.parser.get_suites(values, changed_tests)
        tests_by_executor = create_executor_list(suites)
        if values.report_file is not None:
            _write_report_file(tests_by_executor, values.report_file)

    # If we're not in noExec mode, run the tests.
    if not values.no_exec:
        test_results = {"failures": 0, "results": []}

        # This is a temporary workaround until we have better exclusion semantics.  Check for
        # enterprise executors and the existence of the ekf2 file in the enterprise modules dir.
        executor_black_list = ["audit", "ese", "rlp", "sasl", "snmp"]
        ekf2_file = "src/mongo/db/modules/enterprise/jstests/encryptdb/libs/ekf2"
        for executor in sorted(tests_by_executor):
            for bl_entry in executor_black_list:
                if bl_entry in executor:
                    if values.no_enterprise:
                        tests_by_executor.pop(executor)
                        print "Skipping executor", executor
                    elif not os.path.isfile(ekf2_file):
                        print "The mongo enterprise module is not installed.", \
                              "You may specify the --skipEnterpriseSuites flag to skip these" \
                              "test executors, or run against an enterprise build."
                        sys.exit(1)
                    else:
                        # We have the files to run enterprise executors.
                        break

        for executor in sorted(tests_by_executor):
            test_names = tests_by_executor[executor]
            try:
                subprocess.check_call(" ".join(args) +
                                      " --executor " + executor + " " +
                                      " ".join(test_names), shell=True)
            except subprocess.CalledProcessError as err:
                print "Resmoke returned an error with executor:", executor
                _save_report_data(test_results, "report.json")
                _write_report_file(test_results, "report.json")
                sys.exit(err.returncode)

            _save_report_data(test_results, "report.json")
        _write_report_file(test_results, "report.json")

    sys.exit(0)

if __name__ == "__main__":
    main()
