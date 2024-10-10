#!/usr/bin/env python3
"""Utility to validate resmoke tests runtime."""

import json
import sys
from collections import namedtuple
from statistics import mean
from typing import Dict, List

import click
import structlog

from buildscripts.resmokelib.testing.report import TestInfo, TestReport
from buildscripts.resmokelib.utils import get_task_name_without_suffix
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.teststats import HistoricalTestInformation, HistoricTaskData

LOGGER = structlog.get_logger("buildscripts.resmoke_tests_runtime_validate")

LOOK_BACK_NUM_DAYS = 20
BURN_IN_PREFIX = "burn_in:"

HISTORIC_MAX_MULTIPLIER = 1.5
IGNORE_LESS_THAN_SECS = 15

_TestData = namedtuple("TestData", ["test_file", "duration"])


def parse_resmoke_report(report_file: str) -> List[TestInfo]:
    """Get js tests info from resmoke report json."""
    with open(report_file, "r") as fh:
        report_data = json.load(fh)
    test_report = TestReport.from_dict(report_data)
    return [test_info for test_info in test_report.test_infos if "jstests" in test_info.test_file]


def get_historic_stats(
    project_id: str, task_name: str, build_variant: str
) -> List[HistoricalTestInformation]:
    """Get historic test stats."""
    base_task_name = get_task_name_without_suffix(task_name, build_variant).replace(
        BURN_IN_PREFIX, ""
    )
    return HistoricTaskData.get_stats_from_s3(project_id, base_task_name, build_variant)


def make_stats_map(stats: List[_TestData]) -> Dict[str, List[float]]:
    """Make test stats map."""
    stats_map = {}

    for stat in stats:
        if stat.test_file in stats_map:
            stats_map[stat.test_file].append(stat.duration)
        else:
            stats_map[stat.test_file] = [stat.duration]

    return stats_map


@click.command()
@click.option(
    "--resmoke-report-file", type=str, required=True, help="Location of resmoke's report JSON file."
)
@click.option("--project-id", type=str, required=True, help="Evergreen project id.")
@click.option("--build-variant", type=str, required=True, help="Evergreen build variant name.")
@click.option("--task-name", type=str, required=True, help="Evergreen task name.")
def main(resmoke_report_file: str, project_id: str, build_variant: str, task_name: str) -> None:
    """Compare resmoke tests runtime with historic stats."""
    enable_logging(verbose=False)

    current_test_infos = parse_resmoke_report(resmoke_report_file)
    current_stats_map = make_stats_map(
        [
            _TestData(test_info.test_file, test_info.end_time - test_info.start_time)
            for test_info in current_test_infos
        ]
    )

    historic_stats = get_historic_stats(project_id, task_name, build_variant)
    historic_stats_map = make_stats_map(
        [
            _TestData(test_stats.test_name, test_stats.avg_duration_pass)
            for test_stats in historic_stats
        ]
    )

    failed = False

    for test, stats in current_stats_map.items():
        current_mean = mean(stats)
        if current_mean < IGNORE_LESS_THAN_SECS:
            continue

        historic_test_stats = historic_stats_map.get(test)
        if historic_test_stats:
            historic_max = max(historic_test_stats)
            target = historic_max * HISTORIC_MAX_MULTIPLIER
            if current_mean > target:
                LOGGER.error(
                    "Found long running test.",
                    test_file=test,
                    current_mean_time=current_mean,
                    maximum_expected_time=target,
                    historic_max_time=historic_max,
                )
                failed = True

    LOGGER.info("Done comparing resmoke tests runtime with historic stats.")
    if failed:
        percent = int((HISTORIC_MAX_MULTIPLIER - 1) * 100)
        LOGGER.error(
            f"The test failed due to its runtime taking {percent}% more than the recent max"
            " and can negatively contribute to the future patch build experience."
            " Consider checking if there is an unexpected regression."
        )
        LOGGER.error(
            "If the test is being intentionally expanded, please split it up into separate"
            " JS files that run as separate tests."
        )
        LOGGER.error(
            "If you believe the test has inherently large variability, please consider writing"
            " a new test instead of modifying this one."
        )
        LOGGER.error("For any other questions or concerns, please reach out to #server-testing.")
        sys.exit(1)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
