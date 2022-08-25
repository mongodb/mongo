"""Service for determining task timeouts."""
from datetime import datetime
from typing import Any, Dict, NamedTuple, Optional

import inject
import structlog
from buildscripts.resmoke_proxy.resmoke_proxy import ResmokeProxyService
from buildscripts.timeouts.timeout import TimeoutEstimate
from buildscripts.util.teststats import HistoricTaskData
from evergreen import EvergreenApi

LOGGER = structlog.get_logger(__name__)
CLEAN_EVERY_N_HOOK = "CleanEveryN"


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


class TimeoutSettings(NamedTuple):
    """Settings for determining timeouts."""

    start_date: datetime
    end_date: datetime


class TimeoutService:
    """A service for determining task timeouts."""

    @inject.autoparams()
    def __init__(self, evg_api: EvergreenApi, resmoke_proxy: ResmokeProxyService,
                 timeout_settings: TimeoutSettings) -> None:
        """
        Initialize the service.

        :param evg_api: Evergreen API client.
        :param resmoke_proxy: Proxy to query resmoke.
        :param timeout_settings: Settings for how timeouts are calculated.
        """
        self.evg_api = evg_api
        self.resmoke_proxy = resmoke_proxy
        self.timeout_settings = timeout_settings

    def get_timeout_estimate(self, timeout_params: TimeoutParams) -> TimeoutEstimate:
        """
        Calculate the timeout estimate for the given task based on historic test results.

        :param timeout_params: Details about the task to query.
        :return: Timeouts to use based on historic test results.
        """
        historic_stats = self.lookup_historic_stats(timeout_params)
        if not historic_stats:
            return TimeoutEstimate.no_timeouts()

        test_set = set(self.resmoke_proxy.list_tests(timeout_params.suite_name))
        test_runtimes = [
            stat for stat in historic_stats.get_tests_runtimes() if stat.test_name in test_set
        ]
        test_runtime_set = {test.test_name for test in test_runtimes}
        for test in test_set:
            if test not in test_runtime_set:
                # If we don't have historic runtime information for all the tests, we cannot
                # reliable determine a timeout, so fallback to a default timeout.
                LOGGER.warning(
                    "Could not find historic runtime information for test, using default timeout",
                    test=test)
                return TimeoutEstimate.no_timeouts()

        total_runtime = 0.0
        max_runtime = 0.0

        for runtime in test_runtimes:
            if runtime.runtime > 0.0:
                total_runtime += runtime.runtime
                max_runtime = max(max_runtime, runtime.runtime)
            else:
                LOGGER.warning("Found a test with 0 runtime, using default timeouts",
                               test=runtime.test_name)
                # We found a test with a runtime of 0, which indicates that it does not have a
                # proper runtime history, so fall back to a default timeout.
                return TimeoutEstimate.no_timeouts()

        hook_overhead = self.get_task_hook_overhead(
            timeout_params.suite_name, timeout_params.is_asan, len(test_set), historic_stats)
        total_runtime += hook_overhead

        return TimeoutEstimate(max_test_runtime=max_runtime, expected_task_runtime=total_runtime)

    def get_task_hook_overhead(self, suite_name: str, is_asan: bool, test_count: int,
                               historic_stats: Optional[HistoricTaskData]) -> float:
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
        LOGGER.debug("task hook overhead", cadence=clean_every_n_cadence,
                     runtime=avg_clean_every_n_runtime, is_asan=is_asan)
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
            evg_stats = HistoricTaskData.from_evg(
                self.evg_api, timeout_params.evg_project, self.timeout_settings.start_date,
                self.timeout_settings.end_date, timeout_params.task_name,
                timeout_params.build_variant)
            if not evg_stats:
                LOGGER.warning("No historic runtime information available")
                return None
            return evg_stats
        except Exception:  # pylint: disable=broad-except
            # If we have any trouble getting the historic runtime information, log the issue, but
            # don't fall back to default timeouts instead of failing.
            LOGGER.warning("Error querying history runtime information from evergreen",
                           exc_info=True)
            return None

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
        hooks_config = self.resmoke_proxy.read_suite_config(suite_name).get("executor",
                                                                            {}).get("hooks")
        if hooks_config:
            for hook in hooks_config:
                if hook.get("class") == hook_name:
                    return hook

        return None
