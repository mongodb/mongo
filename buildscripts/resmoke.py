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
    Executes the test suite, failing fast if requested.

    Returns true if the execution of the suite was interrupted, and false otherwise.
    """

    logger = resmokelib.logging.loggers.EXECUTOR_LOGGER

    if resmokelib.config.SHUFFLE:
        logger.info("Shuffling order of tests for %ss in suite %s. The seed is %d.",
                    suite.test_kind, suite.get_display_name(), resmokelib.config.RANDOM_SEED)
        random.seed(resmokelib.config.RANDOM_SEED)
        random.shuffle(suite.tests)

    if resmokelib.config.DRY_RUN == "tests":
        sb = []
        sb.append("Tests that would be run for %ss in suite %s:"
                  % (suite.test_kind, suite.get_display_name()))
        if len(suite.tests) > 0:
            for test in suite.tests:
                sb.append(test)
        else:
            sb.append("(no tests)")
        logger.info("\n".join(sb))

        # Set a successful return code on the test suite because we want to output the tests
        # that would get run by any other suites the user specified.
        suite.return_code = 0
        return False

    if len(suite.tests) == 0:
        logger.info("Skipping %ss, no tests to run", suite.test_kind)

        # Set a successful return code on the test suite because we want to output the tests
        # that would get run by any other suites the user specified.
        suite.return_code = 0
        return False

    archive = None
    if resmokelib.config.ARCHIVE_FILE:
        archive = resmokelib.utils.archival.Archival(
            archival_json_file=resmokelib.config.ARCHIVE_FILE,
            limit_size_mb=resmokelib.config.ARCHIVE_LIMIT_MB,
            limit_files=resmokelib.config.ARCHIVE_LIMIT_TESTS,
            logger=logger)

    executor_config = suite.get_executor_config()

    try:
        executor = resmokelib.testing.executor.TestSuiteExecutor(
            logger, suite, archive_instance=archive, **executor_config)
        executor.run()
        if suite.options.fail_fast and suite.return_code != 0:
            return False
    except (resmokelib.errors.UserInterrupt, resmokelib.errors.LoggerRuntimeConfigError) as err:
        logger.error("Encountered an error when running %ss of suite %s: %s",
                     suite.test_kind, suite.get_display_name(), err)
        suite.return_code = err.EXIT_CODE
        return True
    except:
        logger.exception("Encountered an error when running %ss of suite %s.",
                         suite.test_kind, suite.get_display_name())
        suite.return_code = 2
        return False
    finally:
        if archive:
            archive.exit()


def _log_summary(logger, suites, time_taken):
    if len(suites) > 1:
        resmokelib.testing.suite.Suite.log_summaries(logger, suites, time_taken)


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
    sb.append("YAML configuration of suite %s" % (suite.get_display_name()))
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
        for test in suite.tests:
            memberships[test] = test_membership[test]
    return memberships


def _list_suites_and_exit(logger, exit_code=0):
    suite_names = resmokelib.parser.get_named_suites()
    logger.info("Suites available to execute:\n%s", "\n".join(suite_names))
    sys.exit(exit_code)


class Main(object):
    """
    A class for executing potentially multiple resmoke.py test suites.
    """

    def __init__(self):
        """
        Initializes the Main instance by parsing the command line arguments.
        """

        self.__start_time = time.time()

        values, args = resmokelib.parser.parse_command_line()
        self.__values = values
        self.__args = args

    def _get_suites(self):
        """
        Returns a list of resmokelib.testing.suite.Suite instances to execute.
        """

        return resmokelib.parser.get_suites(self.__values, self.__args)

    def run(self):
        """
        Executes the list of resmokelib.testing.suite.Suite instances returned by _get_suites().
        """

        logging_config = resmokelib.parser.get_logging_config(self.__values)
        resmokelib.logging.loggers.configure_loggers(logging_config)
        resmokelib.logging.flush.start_thread()

        resmokelib.parser.update_config_vars(self.__values)

        exec_logger = resmokelib.logging.loggers.EXECUTOR_LOGGER
        resmoke_logger = exec_logger.new_resmoke_logger()

        if self.__values.list_suites:
            _list_suites_and_exit(resmoke_logger)

        # Log the command line arguments specified to resmoke.py to make it easier to re-run the
        # resmoke.py invocation used by an Evergreen task.
        resmoke_logger.info("resmoke.py invocation: %s", " ".join(sys.argv))

        interrupted = False
        try:
            suites = self._get_suites()
        except resmokelib.errors.SuiteNotFound as err:
            resmoke_logger.error("Failed to parse YAML suite definition: %s", str(err))
            _list_suites_and_exit(resmoke_logger, exit_code=1)

        # Register a signal handler or Windows event object so we can write the report file if the
        # task times out.
        resmokelib.sighandler.register(resmoke_logger, suites, self.__start_time)

        # Run the suite finder after the test suite parsing is complete.
        if self.__values.find_suites:
            suites_by_test = find_suites_by_test(suites)
            for test in sorted(suites_by_test):
                suite_names = suites_by_test[test]
                resmoke_logger.info("%s will be run by the following suite(s): %s",
                                    test, suite_names)
            sys.exit(0)

        exit_code = 0
        try:
            for suite in suites:
                resmoke_logger.info(_dump_suite_config(suite, logging_config))

                suite.record_suite_start()
                interrupted = _execute_suite(suite)
                suite.record_suite_end()

                resmoke_logger.info("=" * 80)
                resmoke_logger.info("Summary of %s suite: %s",
                                    suite.get_display_name(), _summarize_suite(suite))

                if interrupted or (suite.options.fail_fast and suite.return_code != 0):
                    time_taken = time.time() - self.__start_time
                    _log_summary(resmoke_logger, suites, time_taken)
                    exit_code = suite.return_code
                    sys.exit(exit_code)

            time_taken = time.time() - self.__start_time
            _log_summary(resmoke_logger, suites, time_taken)

            # Exit with a nonzero code if any of the suites failed.
            exit_code = max(suite.return_code for suite in suites)
            sys.exit(exit_code)
        finally:
            if not interrupted:
                resmokelib.logging.flush.stop_thread()

            resmokelib.reportfile.write(suites)

            if not interrupted and resmokelib.logging.buildlogger.is_log_output_incomplete():
                if exit_code == 0:
                    # We don't anticipate users to look at passing Evergreen tasks very often that
                    # even if the log output is incomplete, we'd still rather not show anything in
                    # the Evergreen UI or cause a JIRA ticket to be created.
                    resmoke_logger.info(
                        "We failed to flush all log output to logkeeper but all tests passed, so"
                        " ignoring.")
                else:
                    resmoke_logger.info(
                        "Exiting with code %d rather than requested code %d because we failed to"
                        " flush all log output to logkeeper.",
                        resmokelib.errors.LoggerRuntimeConfigError.EXIT_CODE, exit_code)
                    sys.exit(resmokelib.errors.LoggerRuntimeConfigError.EXIT_CODE)

if __name__ == "__main__":
    Main().run()
