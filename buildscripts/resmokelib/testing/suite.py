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

    TESTS_ORDER = ("cpp_unit_test", "cpp_integration_test", "db_test", "js_test", "mongos_test")

    def __init__(self, suite_name, suite_config):
        """
        Initializes the suite with the specified name and configuration.
        """

        self._suite_name = suite_name
        self._suite_config = suite_config

        self.test_groups = []
        for test_kind in Suite.TESTS_ORDER:
            if test_kind not in suite_config["selector"]:
                continue
            tests = self._get_tests_for_group(test_kind)
            test_group = testgroup.TestGroup(test_kind, tests)
            self.test_groups.append(test_group)

        self.return_code = None

        self._start_time = None
        self._end_time = None

    def _get_tests_for_group(self, test_kind):
        """
        Returns the tests to run based on the 'test_kind'-specific
        filtering policy.
        """

        test_info = self.get_selector_config()[test_kind]

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
            # The return code of the suite should be 2 if any test group has a return code of 2.
            # The return code of the suite should be 1 if any test group has a return code of 1,
            # and none have a return code of 2. Otherwise, the return code should be 0.
            self.return_code = max(test_group.return_code for test_group in self.test_groups)

    def summarize(self, sb):
        """
        Appends a summary of each individual test group onto the string
        builder 'sb'.
        """

        combined_summary = _summary.Summary(0, 0.0, 0, 0, 0, 0)

        summarized_groups = []
        for group in self.test_groups:
            group_sb = []
            summary = group.summarize(group_sb)
            summarized_groups.append("    %ss: %s" % (group.test_kind, "\n        ".join(group_sb)))

            combined_summary = _summary.combine(combined_summary, summary)

        if combined_summary.num_run == 0:
            sb.append("Suite did not run any tests.")
            return

        # Override the 'time_taken' attribute of the summary if we have more accurate timing
        # information available.
        if self._start_time is not None and self._end_time is not None:
            time_taken = self._end_time - self._start_time
            combined_summary = combined_summary._replace(time_taken=time_taken)

        sb.append("%d test(s) ran in %0.2f seconds"
                  " (%d succeeded, %d were skipped, %d failed, %d errored)" % combined_summary)

        for summary_text in summarized_groups:
            sb.append(summary_text)
