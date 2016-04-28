#!/usr/bin/env python

"""
Command line utility for executing MongoDB tests of all kinds.
"""

from __future__ import absolute_import

import json
import os.path
import random
import signal
import sys
import time
import traceback

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from buildscripts import resmokelib


def _execute_suite(suite, logging_config):
    """
    Executes each test group of 'suite', failing fast if requested.

    Returns true if the execution of the suite was interrupted by the
    user, and false otherwise.
    """

    logger = resmokelib.logging.loggers.EXECUTOR

    for group in suite.test_groups:
        if resmokelib.config.SHUFFLE:
            logger.info("Shuffling order of tests for %ss in suite %s. The seed is %d.",
                        group.test_kind, suite.get_name(), resmokelib.config.RANDOM_SEED)
            random.seed(resmokelib.config.RANDOM_SEED)
            random.shuffle(group.tests)

        if resmokelib.config.DRY_RUN == "tests":
            sb = []
            sb.append("Tests that would be run for %ss in suite %s:"
                      % (group.test_kind, suite.get_name()))
            if len(group.tests) > 0:
                for test in group.tests:
                    sb.append(test)
            else:
                sb.append("(no tests)")
            logger.info("\n".join(sb))

            # Set a successful return code on the test group because we want to output the tests
            # that would get run by any other suites the user specified.
            group.return_code = 0
            continue

        if len(group.tests) == 0:
            logger.info("Skipping %ss, no tests to run", group.test_kind)
            continue

        group_config = suite.get_executor_config().get(group.test_kind, {})
        executor = resmokelib.testing.executor.TestGroupExecutor(logger,
                                                                 group,
                                                                 logging_config,
                                                                 **group_config)

        try:
            executor.run()
            if resmokelib.config.FAIL_FAST and group.return_code != 0:
                suite.return_code = group.return_code
                return False
        except resmokelib.errors.UserInterrupt:
            suite.return_code = 130  # Simulate SIGINT as exit code.
            return True
        except:
            logger.exception("Encountered an error when running %ss of suite %s.",
                             group.test_kind, suite.get_name())
            suite.return_code = 2
            return False


def _log_summary(logger, suites, time_taken):
    if len(suites) > 1:
        sb = []
        sb.append("Summary of all suites: %d suites ran in %0.2f seconds"
                  % (len(suites), time_taken))
        for suite in suites:
            suite_sb = []
            suite.summarize(suite_sb)
            sb.append("    %s: %s" % (suite.get_name(), "\n    ".join(suite_sb)))

        logger.info("=" * 80)
        logger.info("\n".join(sb))


def _summarize_suite(suite):
    sb = []
    suite.summarize(sb)
    return "\n".join(sb)


def _dump_suite_config(suite, logging_config):
    """
    Returns a string that represents the YAML configuration of a suite.

    TODO: include the "options" key in the result
    """

    sb = []
    sb.append("YAML configuration of suite %s" % (suite.get_name()))
    sb.append(resmokelib.utils.dump_yaml({"selector": suite.get_selector_config()}))
    sb.append("")
    sb.append(resmokelib.utils.dump_yaml({"executor": suite.get_executor_config()}))
    sb.append("")
    sb.append(resmokelib.utils.dump_yaml({"logging": logging_config}))
    return "\n".join(sb)


def find_suites_by_test(suites):
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

def _write_report_file(suites, pathname):
    """
    Writes the report.json file if requested.
    """

    reports = []
    for suite in suites:
        for group in suite.test_groups:
            reports.extend(group.get_reports())

    combined_report_dict = resmokelib.testing.report.TestReport.combine(*reports).as_dict()
    with open(pathname, "w") as fp:
        json.dump(combined_report_dict, fp)


def main():
    start_time = time.time()

    values, args = resmokelib.parser.parse_command_line()

    logging_config = resmokelib.parser.get_logging_config(values)
    resmokelib.logging.config.apply_config(logging_config)
    resmokelib.logging.flush.start_thread()

    resmokelib.parser.update_config_vars(values)

    exec_logger = resmokelib.logging.loggers.EXECUTOR
    resmoke_logger = resmokelib.logging.loggers.new_logger("resmoke", parent=exec_logger)

    if values.list_suites:
        suite_names = resmokelib.parser.get_named_suites()
        resmoke_logger.info("Suites available to execute:\n%s", "\n".join(suite_names))
        sys.exit(0)

    interrupted = False
    suites = resmokelib.parser.get_suites(values, args)

    # Run the suite finder after the test suite parsing is complete.
    if values.find_suites:
        suites_by_test = find_suites_by_test(suites)
        for test in sorted(suites_by_test):
            suite_names = suites_by_test[test]
            resmoke_logger.info("%s will be run by the following suite(s): %s", test, suite_names)
        sys.exit(0)

    try:
        for suite in suites:
            resmoke_logger.info(_dump_suite_config(suite, logging_config))

            suite.record_start()
            interrupted = _execute_suite(suite, logging_config)
            suite.record_end()

            resmoke_logger.info("=" * 80)
            resmoke_logger.info("Summary of %s suite: %s",
                                suite.get_name(), _summarize_suite(suite))

            if interrupted or (resmokelib.config.FAIL_FAST and suite.return_code != 0):
                time_taken = time.time() - start_time
                _log_summary(resmoke_logger, suites, time_taken)
                sys.exit(suite.return_code)

        time_taken = time.time() - start_time
        _log_summary(resmoke_logger, suites, time_taken)

        # Exit with a nonzero code if any of the suites failed.
        exit_code = max(suite.return_code for suite in suites)
        sys.exit(exit_code)
    finally:
        if not interrupted:
            resmokelib.logging.flush.stop_thread()

        if resmokelib.config.REPORT_FILE is not None:
            _write_report_file(suites, resmokelib.config.REPORT_FILE)


if __name__ == "__main__":

    def _dump_stacks(signum, frame):
        """
        Signal handler that will dump the stacks of all threads.
        """

        header_msg = "Dumping stacks due to SIGUSR1 signal"

        sb = []
        sb.append("=" * len(header_msg))
        sb.append(header_msg)
        sb.append("=" * len(header_msg))

        frames = sys._current_frames()
        sb.append("Total threads: %d" % (len(frames)))
        sb.append("")

        for thread_id in frames:
            stack = frames[thread_id]
            sb.append("Thread %d:" % (thread_id))
            sb.append("".join(traceback.format_stack(stack)))

        sb.append("=" * len(header_msg))
        print "\n".join(sb)

    try:
        signal.signal(signal.SIGUSR1, _dump_stacks)
    except AttributeError:
        print "Cannot catch signals on Windows"

    main()
