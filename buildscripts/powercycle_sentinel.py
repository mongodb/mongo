#!/usr/bin/env python3
"""Powercycle tasks sentinel.

Error out when any powercycle task on the same buildvariant runs for more than 2 hours.
"""

import logging
import os
import sys
import time
from datetime import datetime, timezone
from typing import List

import click
import structlog

from buildscripts.util.read_config import read_config_file
from evergreen import EvergreenApi, RetryingEvergreenApi

LOGGER = structlog.getLogger(__name__)

EVERGREEN_HOST = "https://evergreen.mongodb.com"
EVERGREEN_CONFIG_LOCATIONS = (
    # Common for machines in Evergreen
    os.path.join(os.getcwd(), ".evergreen.yml"),
    # Common for local machines
    os.path.expanduser(os.path.join("~", ".evergreen.yml")),
)
POWERCYCLE_TASK_EXEC_TIMEOUT_SECS = 2 * 60 * 60
WATCH_INTERVAL_SECS = 5 * 60


def get_evergreen_api() -> EvergreenApi:
    """Return evergreen API."""
    # Pickup the first config file found in common locations.
    for file in EVERGREEN_CONFIG_LOCATIONS:
        if os.path.isfile(file):
            evg_api = RetryingEvergreenApi.get_api(config_file=file)
            return evg_api

    LOGGER.error("Evergreen config not found in locations.", locations=EVERGREEN_CONFIG_LOCATIONS)
    sys.exit(1)


def watch_tasks(task_ids: List[str], evg_api: EvergreenApi, watch_interval_secs: int) -> List[str]:
    """Watch tasks if they run longer than exec timeout."""
    watch_task_ids = task_ids[:]
    long_running_task_ids = []

    while watch_task_ids:
        LOGGER.info("Looking if powercycle tasks are still running on the current buildvariant.")
        powercycle_tasks = [evg_api.task_by_id(task_id) for task_id in watch_task_ids]
        for task in powercycle_tasks:
            if task.finish_time:
                watch_task_ids.remove(task.task_id)
            elif (
                task.start_time
                and (datetime.now(timezone.utc) - task.start_time).total_seconds()
                > POWERCYCLE_TASK_EXEC_TIMEOUT_SECS
            ):
                long_running_task_ids.append(task.task_id)
                watch_task_ids.remove(task.task_id)
        if watch_task_ids:
            time.sleep(watch_interval_secs)

    return long_running_task_ids


def get_links(task_ids: List[str]) -> str:
    """Return evergreen task urls delimited by newline."""
    return "\n".join([f"{EVERGREEN_HOST}/task/{task_id}" for task_id in task_ids])


@click.command()
@click.argument("expansions_file", type=str, default="expansions.yml")
def main(expansions_file: str = "expansions.yml") -> None:
    """Implementation."""

    logging.basicConfig(
        format="[%(levelname)s] %(message)s",
        level=logging.INFO,
        stream=sys.stdout,
    )
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())

    expansions = read_config_file(expansions_file)
    build_id = expansions["build_id"]
    current_task_id = expansions["task_id"]
    gen_task_name = expansions["gen_task"]

    evg_api = get_evergreen_api()

    build_tasks = evg_api.tasks_by_build(build_id)
    gen_task_id = [task.task_id for task in build_tasks if gen_task_name in task.task_id][0]
    gen_task_url = f"{EVERGREEN_HOST}/task/{gen_task_id}"

    while evg_api.task_by_id(gen_task_id).is_active():
        LOGGER.info(
            f"Waiting for '{gen_task_name}' task to generate powercycle tasks:\n{gen_task_url}"
        )
        time.sleep(WATCH_INTERVAL_SECS)

    build_tasks = evg_api.tasks_by_build(build_id)
    powercycle_task_ids = [
        task.task_id
        for task in build_tasks
        if not task.display_only
        and task.task_id != current_task_id
        and task.task_id != gen_task_id
        and "powercycle" in task.task_id
    ]
    LOGGER.info(f"Watching powercycle tasks:\n{get_links(powercycle_task_ids)}")

    long_running_task_ids = watch_tasks(powercycle_task_ids, evg_api, WATCH_INTERVAL_SECS)
    if long_running_task_ids:
        LOGGER.error(
            f"Found powercycle tasks that are running for more than {POWERCYCLE_TASK_EXEC_TIMEOUT_SECS} "
            f"seconds and most likely something is going wrong in those tasks:\n{get_links(long_running_task_ids)}"
        )
        LOGGER.error(
            "Hopefully hosts from the tasks are still in run at the time you are seeing this "
            "and the Build team is able to check them to diagnose the issue."
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
