#!/usr/bin/env python3
"""Analyze performance results of Google Benchmarks."""

import sys

import click
import structlog
from requests import HTTPError
from evergreen import RetryingEvergreenApi

from buildscripts.benchmarks.compare import compare_data
from buildscripts.util.cedar_api import CedarApi
from buildscripts.util.cmdutils import enable_logging

LOGGER = structlog.get_logger("buildscripts.benchmarks.analyze")


def get_baseline_task_id(evg_api_config: str, current_task_id: str) -> str:
    """Get the most recent successful task prior to the current task."""
    evg_api = RetryingEvergreenApi.get_api(config_file=evg_api_config)
    current_task = evg_api.task_by_id(current_task_id)

    base_version = evg_api.version_by_id(
        f"{current_task.json['project_identifier'].replace('-', '_')}_{current_task.revision}")
    prior_versions = evg_api.versions_by_project(current_task.project_id, start=base_version.order)

    for version in prior_versions:
        baseline_task_candidate = None

        try:
            build = version.build_by_variant(current_task.build_variant)
        # If previous version doesn't contain the build variant of the current task
        # assume that it's not possible to find the baseline task
        except KeyError:
            return ""

        for task in build.get_tasks():
            if task.display_name == current_task.display_name:
                baseline_task_candidate = task
                break

        # If previous build doesn't contain the task with the same 'display_name'
        # assume that this is a new task and there is no baseline task
        if baseline_task_candidate is None:
            return ""

        if baseline_task_candidate.is_success():
            return baseline_task_candidate.task_id

    return ""


@click.command()
@click.option("--evg-api-config", type=str, required=True,
              help="Location of evergreen api configuration.")
@click.option("--task-id", type=str, required=True, help="Current evergreen task id.")
@click.option("--suite", type=str, required=True, help="Resmoke suite name.")
def main(evg_api_config: str, task_id: str, suite: str) -> None:
    """Analyze performance results of Google Benchmarks."""
    enable_logging(verbose=False)

    LOGGER.info("Looking for a baseline task...")
    baseline_task_id = get_baseline_task_id(evg_api_config, task_id)
    if baseline_task_id:
        LOGGER.info("Found baseline task.", task_id=baseline_task_id)
    else:
        LOGGER.warning("")
        LOGGER.warning("Baseline task not found in Evergreen.")
        LOGGER.warning("If you think that this is unexpected,"
                       " please reach out to #server-testing")
        LOGGER.warning("")

    LOGGER.info("Getting performance data...")
    cedar_api = CedarApi(evg_api_config)
    current_data = cedar_api.get_perf_data_by_task_id(task_id)
    baseline_data = []

    try:
        baseline_data = cedar_api.get_perf_data_by_task_id(baseline_task_id)
    # Swallow HTTPError, since for a new benchmark there might not be historic perf data
    except HTTPError as err:
        if baseline_task_id:
            LOGGER.warning("")
            LOGGER.warning("Could not get performance data for a baseline task from Cedar",
                           task_id=baseline_task_id)
            LOGGER.warning("", error=err)
            LOGGER.warning("If you think that this is unexpected,"
                           " please reach out to #performance-tooling-users")
            LOGGER.warning("")

    LOGGER.info("Comparing the current performance data with a baseline data.")
    result = compare_data(suite, current_data, baseline_data)
    LOGGER.info(f"Performance analysis result:\n{result}")
    if not result.passed:
        LOGGER.error("Performance data delta has exceeded threshold.")
        sys.exit(1)


if __name__ == '__main__':
    main()  # pylint: disable=no-value-for-parameter
