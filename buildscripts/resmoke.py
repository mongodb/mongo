#!/usr/bin/env python

"""
Command line utility for executing MongoDB tests of all kinds.
"""

from __future__ import absolute_import

import os.path
import random
import sys
import time

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from buildscripts import resmokelib


def _execute_suite(suite):
    """
    Executes each test group of 'suite', failing fast if requested.

    Returns true if the execution of the suite was interrupted by the
    user, and false otherwise.
    """

    logger = resmokelib.logging.loggers.EXECUTOR_LOGGER

    if resmokelib.config.SHUFFLE:
        logger.info("Shuffling order of tests for %ss in suite %s. The seed is %d.",
                    suite.test_group.test_kind, suite.get_name(), resmokelib.config.RANDOM_SEED)
        random.seed(resmokelib.config.RANDOM_SEED)
        random.shuffle(suite.test_group.tests)

    if resmokelib.config.DRY_RUN == "tests":
        sb = []
        sb.append("Tests that would be run for %ss in suite %s:"
                  % (suite.test_group.test_kind, suite.get_name()))
        if len(suite.test_group.tests) > 0:
            for test in suite.test_group.tests:
                sb.append(test)
        else:
            sb.append("(no tests)")
        logger.info("\n".join(sb))

        # Set a successful return code on the test group because we want to output the tests
        # that would get run by any other suites the user specified.
        suite.test_group.return_code = 0
        return True

    if len(suite.test_group.tests) == 0:
        logger.info("Skipping %ss, no tests to run", suite.test_group.test_kind)
        return True

    group_config = suite.get_executor_config()
    executor = resmokelib.testing.executor.TestGroupExecutor(logger,
                                                             suite.test_group,
                                                             **group_config)

    try:
        executor.run()
        if resmokelib.config.FAIL_FAST and suite.test_group.return_code != 0:
            suite.return_code = suite.test_group.return_code
            return False
    except resmokelib.errors.UserInterrupt:
        suite.return_code = 130  # Simulate SIGINT as exit code.
        return True
    except:
        logger.exception("Encountered an error when running %ss of suite %s.",
                         suite.test_group.test_kind, suite.get_name())
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
    sb.append(resmokelib.utils.dump_yaml({"test_kind": suite.get_test_kind_config()}))
    sb.append("")
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
        for test in suite.test_group.tests:
            memberships[test] = test_membership[test]
    return memberships


def main():
    start_time = time.time()

    values, args = resmokelib.parser.parse_command_line()

    logging_config = resmokelib.parser.get_logging_config(values)
    resmokelib.logging.loggers.configure_loggers(logging_config)
    resmokelib.logging.flush.start_thread()

    resmokelib.parser.update_config_vars(values)

    exec_logger = resmokelib.logging.loggers.EXECUTOR_LOGGER
    resmoke_logger = exec_logger.new_resmoke_logger()

    if values.list_suites:
        suite_names = resmokelib.parser.get_named_suites()
        resmoke_logger.info("Suites available to execute:\n%s", "\n".join(suite_names))
        sys.exit(0)

    # Log the command line arguments specified to resmoke.py to make it easier to re-run the
    # resmoke.py invocation used by an Evergreen task.
    resmoke_logger.info("resmoke.py invocation: %s", " ".join(sys.argv))

    interrupted = False
    suites = resmokelib.parser.get_suites(values, args)

    # Register a signal handler or Windows event object so we can write the report file if the task
    # times out.
    resmokelib.sighandler.register(resmoke_logger, suites)

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
            interrupted = _execute_suite(suite)
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

        resmokelib.reportfile.write(suites)


if __name__ == "__main__":
    main()
