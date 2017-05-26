"""
Holder for a set of TestGroup instances.
"""

from __future__ import absolute_import

import time

from . import summary as _summary
from . import testgroup
from .. import config as _config
from .. import selector as _selector


class Suite(object):
    """
    A suite of tests.
    """

    def __init__(self, suite_name, suite_config):
        """
        Initializes the suite with the specified name and configuration.
        """

        self._suite_name = suite_name
        self._suite_config = suite_config

        test_kind = self.get_test_kind_config()
        tests = self._get_tests_for_group(test_kind)
        self.test_group = testgroup.TestGroup(test_kind, tests)

        self.return_code = None

        self._start_time = None
        self._end_time = None

    def _get_tests_for_group(self, test_kind):
        """
        Returns the tests to run based on the 'test_kind'-specific
        filtering policy.
        """

        test_info = self.get_selector_config()

        # The mongos_test doesn't have to filter anything, the test_info is just the arguments to
        # the mongos program to be used as the test case.
        if test_kind == "mongos_test":
            mongos_options = test_info  # Just for easier reading.
            if not isinstance(mongos_options, dict):
                raise TypeError("Expected dictionary of arguments to mongos")
            return [mongos_options]
        elif test_kind == "cpp_integration_test":
            tests = _selector.filter_cpp_integration_tests(**test_info)
        elif test_kind == "cpp_unit_test":
            tests = _selector.filter_cpp_unit_tests(**test_info)
        elif test_kind == "db_test":
            tests = _selector.filter_dbtests(**test_info)
        else:  # test_kind == "js_test":
            tests = _selector.filter_jstests(**test_info)

        if _config.ORDER_TESTS_BY_NAME:
            return sorted(tests, key=str.lower)
        return tests

    def get_name(self):
        """
        Returns the name of the test suite.
        """
        return self._suite_name

    def get_selector_config(self):
        """
        Returns the "selector" section of the YAML configuration.
        """
        return self._suite_config["selector"]

    def get_executor_config(self):
        """
        Returns the "executor" section of the YAML configuration.
        """
        return self._suite_config["executor"]

    def get_test_kind_config(self):
        """
        Returns the "test_kind" section of the YAML configuration.
        """
        return self._suite_config["test_kind"]

    def record_start(self):
        """
        Records the start time of the suite.
        """
        self._start_time = time.time()

    def record_end(self):
        """
        Records the end time of the suite.

        Sets the 'return_code' of the suite based on the record codes of
        each of the individual test groups.
        """

        self._end_time = time.time()

        # Only set 'return_code' if it hasn't been set already. It may have been set if there was
        # an exception that happened during the execution of the suite.
        if self.return_code is None:
            self.return_code = self.test_group.return_code

    def summarize(self, sb):
        """
        Appends a summary of the test group onto the string builder 'sb'.
        """

        summary = _summary.Summary(0, 0.0, 0, 0, 0, 0)

        summary = self.test_group.summarize(sb)
        summarized_group = "    %ss: %s" % (self.test_group.test_kind, "\n        ".join(sb))

        if summary.num_run == 0:
            sb.append("Suite did not run any tests.")
            return

        # Override the 'time_taken' attribute of the summary if we have more accurate timing
        # information available.
        if self._start_time is not None and self._end_time is not None:
            time_taken = self._end_time - self._start_time
            summary = summary._replace(time_taken=time_taken)

        sb.append("%d test(s) ran in %0.2f seconds"
                  " (%d succeeded, %d were skipped, %d failed, %d errored)" % summary)

        sb.append(summarized_group)
