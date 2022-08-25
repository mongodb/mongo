"""A service to proxy requests to resmoke."""
from typing import List, Dict, Any

import inject

import buildscripts.resmokelib.parser as _parser
import buildscripts.resmokelib.suitesconfig as _suiteconfig


class ResmokeProxyService:
    """A service to proxy requests to resmoke."""

    @inject.autoparams()
    def __init__(self, run_options="") -> None:
        """Initialize the service."""
        _parser.set_run_options(run_options)
        self._suite_config = _suiteconfig

    def list_tests(self, suite_name: str) -> List[str]:
        """
        List the test files that are part of the suite.

        :param suite_name: Name of suite to query.
        :return: List of test names that belong to the suite.
        """
        suite = self._suite_config.get_suite(suite_name)
        test_list = []
        for tests in suite.tests:
            # `tests` could return individual tests or lists of tests, we need to handle both.
            if isinstance(tests, list):
                test_list.extend(tests)
            else:
                test_list.append(tests)

        return test_list

    def read_suite_config(self, suite_name: str) -> Dict[str, Any]:
        """
        Read the given resmoke suite configuration.

        :param suite_name: Name of suite to read.
        :return: Configuration of specified suite.
        """
        return self._suite_config.SuiteFinder.get_config_obj(suite_name)
