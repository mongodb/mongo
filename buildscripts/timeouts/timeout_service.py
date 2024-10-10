"""Service for determining task timeouts."""

from typing import Any, Dict, NamedTuple, Optional

import inject
import structlog

from buildscripts.resmoke_proxy.resmoke_proxy import ResmokeProxyService
from buildscripts.timeouts.timeout import TimeoutEstimate
from buildscripts.util.teststats import HistoricTaskData, normalize_test_name

LOGGER = structlog.get_logger(__name__)
CLEAN_EVERY_N_HOOK = "CleanEveryN"
REQUIRED_STATS_THRESHOLD = 0.8


class TimeoutParams(NamedTuple):
    """
    Parameters about task being run.

    * evg_project: Evergreen project.
    * build_variant: Evergreen build variant.
    * task_name: Evergreen task_name.
    * suite_name: Test Suite being run.
    * is_asan: Whether this run is part of an asan build.
    """

    evg_project: str
    build_variant: str
    task_name: str
    suite_name: str
    is_asan: bool


class TimeoutService:
    """A service for determining task timeouts."""

    @inject.autoparams()
    def __init__(self, resmoke_proxy: ResmokeProxyService) -> None:
        """
        Initialize the service.

        :param resmoke_proxy: Proxy to query resmoke.
        """
        self.resmoke_proxy = resmoke_proxy

    def get_timeout_estimate(self, timeout_params: TimeoutParams) -> TimeoutEstimate:
        """
        Calculate the timeout estimate for the given task based on historic test results.

        :param timeout_params: Details about the task to query.
        :return: Timeouts to use based on historic test results.
        """
        historic_stats = self.lookup_historic_stats(timeout_params)
        if not historic_stats:
            LOGGER.warning("Missing historic runtime information, using default timeout")
            return TimeoutEstimate.no_timeouts()

        test_set = {
            normalize_test_name(test)
            for test in self.resmoke_proxy.list_tests(timeout_params.suite_name)
        }
        test_runtimes = [
            stat for stat in historic_stats.get_tests_runtimes() if stat.test_name in test_set
        ]
        test_runtime_set = {test.test_name for test in test_runtimes}
        num_tests_missing_historic_data = 0
        for test in test_set:
            if test not in test_runtime_set:
                LOGGER.warning("Could not find historic runtime information for test", test=test)
                num_tests_missing_historic_data += 1

        total_runtime = 0.0
        max_runtime = 0.0

        for runtime in test_runtimes:
            if runtime.runtime > 0.0:
                total_runtime += runtime.runtime
                max_runtime = max(max_runtime, runtime.runtime)
            else:
                LOGGER.warning("Found a test with 0 runtime", test=runtime.test_name)
                num_tests_missing_historic_data += 1

        total_num_tests = len(test_set)
        if not self._have_enough_historic_stats(total_num_tests, num_tests_missing_historic_data):
            LOGGER.warning(
                "Not enough historic runtime information, using default timeout",
                total_num_tests=total_num_tests,
                num_tests_missing_historic_data=num_tests_missing_historic_data,
                required_stats_threshold=REQUIRED_STATS_THRESHOLD,
            )
            return TimeoutEstimate.no_timeouts()

        hook_overhead = self.get_task_hook_overhead(
            timeout_params.suite_name, timeout_params.is_asan, total_num_tests, historic_stats
        )
        total_runtime += hook_overhead

        if num_tests_missing_historic_data > 0:
            total_runtime += num_tests_missing_historic_data * max_runtime
            LOGGER.warning(
                "At least one test misses historic runtime information, using default idle timeout",
                num_tests_missing_historic_data=num_tests_missing_historic_data,
            )
            return TimeoutEstimate.only_task_timeout(expected_task_runtime=total_runtime)

        return TimeoutEstimate(max_test_runtime=max_runtime, expected_task_runtime=total_runtime)

    def get_task_hook_overhead(
        self,
        suite_name: str,
        is_asan: bool,
        test_count: int,
        historic_stats: Optional[HistoricTaskData],
    ) -> float:
        """
        Add how much overhead task-level hooks each suite should account for.

        Certain test hooks need to be accounted for on the task level instead of the test level
        in order to calculate accurate timeouts. So we will add details about those hooks to
        each suite here.

        :param suite_name: Name of suite being generated.
        :param is_asan: Whether ASAN is being used.
        :param test_count: Number of tests in sub-suite.
        :param historic_stats: Historic runtime data of the suite.
        """
        # The CleanEveryN hook is run every 'N' tests. The runtime of the
        # hook will be associated with whichever test happens to be running, which could be
        # different every run. So we need to take its runtime into account at the task level.
        if historic_stats is None:
            return 0.0

        clean_every_n_cadence = self._get_clean_every_n_cadence(suite_name, is_asan)
        avg_clean_every_n_runtime = historic_stats.get_avg_hook_runtime(CLEAN_EVERY_N_HOOK)
        LOGGER.debug(
            "task hook overhead",
            cadence=clean_every_n_cadence,
            runtime=avg_clean_every_n_runtime,
            is_asan=is_asan,
        )
        if avg_clean_every_n_runtime != 0:
            n_expected_runs = test_count / clean_every_n_cadence
            return n_expected_runs * avg_clean_every_n_runtime
        return 0.0

    def lookup_historic_stats(self, timeout_params: TimeoutParams) -> Optional[HistoricTaskData]:
        """
        Lookup historic test results stats for the given task.

        :param timeout_params: Details about the task to lookup.
        :return: Historic test results if they exist.
        """
        try:
            LOGGER.info(
                "Getting historic runtime information",
                evg_project=timeout_params.evg_project,
                build_variant=timeout_params.build_variant,
                task_name=timeout_params.task_name,
            )
            evg_stats = HistoricTaskData.from_s3(
                timeout_params.evg_project, timeout_params.task_name, timeout_params.build_variant
            )
            if not evg_stats:
                LOGGER.warning("No historic runtime information available")
                return None
            LOGGER.info(
                "Found historic runtime information", evg_stats=evg_stats.historic_test_results
            )
            return evg_stats
        except Exception as err:  # pylint: disable=broad-except
            # If we have any trouble getting the historic runtime information, log the issue, but
            # don't fall back to default timeouts instead of failing.
            LOGGER.warning("Error querying history runtime information from evergreen: %s", err)
            return None

    @staticmethod
    def _have_enough_historic_stats(num_tests: int, num_tests_missing_data: int) -> bool:
        """
        Check whether the required number of stats threshold is met.

        :param num_tests: Number of tests to run.
        :param num_tests_missing_data: Number of test that misses historic runtime data.
        :return: Whether the required number of stats threshold is met.
        """
        if num_tests < 0:
            raise ValueError("Number of tests cannot be less than 0")
        if num_tests == 0:
            return True
        return (num_tests - num_tests_missing_data) / num_tests > REQUIRED_STATS_THRESHOLD

    def _get_clean_every_n_cadence(self, suite_name: str, is_asan: bool) -> int:
        """
        Get the N value for the CleanEveryN hook.

        :param suite_name: Name of suite being generated.
        :param is_asan: Whether ASAN is being used.
        :return: How frequently clean every end is run.
        """
        # Default to 1, which is the worst case meaning CleanEveryN would run for every test.
        clean_every_n_cadence = 1
        if is_asan:
            # ASAN runs hard-code N to 1. See `resmokelib/testing/hooks/cleanup.py`.
            return clean_every_n_cadence

        clean_every_n_config = self._get_hook_config(suite_name, CLEAN_EVERY_N_HOOK)
        if clean_every_n_config:
            clean_every_n_cadence = clean_every_n_config.get("n", 1)

        return clean_every_n_cadence

    def _get_hook_config(self, suite_name: str, hook_name: str) -> Optional[Dict[str, Any]]:
        """
        Get the configuration for the given hook.

        :param hook_name: Name of hook to query.
        :return: Configuration for hook, if it exists.
        """
        hooks_config = (
            self.resmoke_proxy.read_suite_config(suite_name).get("executor", {}).get("hooks")
        )
        if hooks_config:
            for hook in hooks_config:
                if hook.get("class") == hook_name:
                    return hook

        return None
