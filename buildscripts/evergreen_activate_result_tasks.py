"""Activate result task groups in the existing build."""

import json
import os
import sys
from typing import Annotated, Optional

import structlog
import typer
from urllib3.util import Retry

from evergreen.api import (
    DEFAULT_HTTP_RETRY_ATTEMPTS,
    DEFAULT_HTTP_RETRY_BACKOFF_FACTOR,
    DEFAULT_HTTP_RETRY_CODES,
    EvergreenApi,
    RetryingEvergreenApi,
)

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file

LOGGER = structlog.getLogger(__name__)

EVG_CONFIG_FILE = "./.evergreen.yml"

app = typer.Typer(pretty_exceptions_show_locals=False)


def get_executed_test_labels(build_events_file: str) -> set[str]:
    """
    Parse a Bazel build events NDJSON file and return all executed test target labels.

    :param build_events_file: Path to the build_events.json NDJSON file.
    :return: Set of Bazel target labels that had test results.
    """
    labels = set()
    with open(build_events_file) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            event = json.loads(line)
            if "testResult" in event:
                label = event["id"]["testResult"]["label"]
                labels.add(label)
    return labels


def activate_result_task_group(
    build_variant: str,
    task_name: str,
    version_id: str,
    evg_api: EvergreenApi,
    build_events_file: Optional[str] = None,
) -> None:
    """
    Activate the result task group for the given variant and task.

    :param build_variant: The build variant name.
    :param task_name: The task name (e.g., "resmoke_tests").
    :param version_id: The Evergreen version ID.
    :param evg_api: Evergreen API client.
    :param build_events_file: Optional path to build_events.json. When provided, asserts that
        every executed test has a corresponding Evergreen task; raises RuntimeError otherwise.
    """
    result_task_group_name = f"{task_name}_results_{build_variant}"

    try:
        version = evg_api.version_by_id(version_id)
        build_id = version.build_variants_map.get(build_variant)

        if not build_id:
            LOGGER.warning(
                "Build variant not found in version",
                build_variant=build_variant,
                version_id=version_id,
            )
            return

        task_list = evg_api.tasks_by_build(build_id)

        # Collect all task names that need activation
        tasks_to_activate = []
        already_activated_count = 0
        evg_task_names = set()
        for task in task_list:
            # Result tasks are bazel targets that start with "//"
            if task.display_name.startswith("//") and "_burn_in_" not in task.display_name:
                evg_task_names.add(task.display_name)
                if task.activated:
                    already_activated_count += 1
                    LOGGER.debug(
                        "Task already activated, skipping",
                        task_id=task.task_id,
                        task_name=task.display_name,
                    )
                else:
                    tasks_to_activate.append(task.display_name)
                    LOGGER.debug(
                        "Found result task to activate",
                        task_id=task.task_id,
                        task_name=task.display_name,
                    )

        if build_events_file:
            executed_labels = get_executed_test_labels(build_events_file)
            missing = executed_labels - evg_task_names
            if missing:
                missing_sorted = sorted(missing)
                LOGGER.error(
                    "Executed tests have no corresponding Evergreen task — "
                    "this indicates a bug in task generation",
                    missing_count=len(missing_sorted),
                    missing_tasks=missing_sorted,
                )
                raise RuntimeError(
                    f"{len(missing_sorted)} executed test(s) have no corresponding Evergreen task: "
                    + ", ".join(missing_sorted)
                )

        if not tasks_to_activate and not already_activated_count:
            LOGGER.warning(
                "No result tasks found to activate",
                task_group=result_task_group_name,
                build_variant=build_variant,
            )
            return

        LOGGER.info(
            "Activating result tasks",
            count=len(tasks_to_activate),
            already_activated=already_activated_count,
            task_group=result_task_group_name,
        )

        variants = [{"name": build_variant, "tasks": tasks_to_activate}]
        evg_api.activate_version_tasks(version_id, variants)

        LOGGER.info(
            "Successfully activated result tasks",
            count=len(tasks_to_activate),
            task_group=result_task_group_name,
        )

    except Exception:
        LOGGER.error(
            "Failed to activate result task group",
            task_group=result_task_group_name,
            build_variant=build_variant,
            version_id=version_id,
            exc_info=True,
        )
        raise


@app.command()
def main(
    expansion_file: Annotated[
        str, typer.Option(help="Location of expansions file generated by evergreen.")
    ],
    evergreen_config: Annotated[
        str, typer.Option(help="Location of evergreen configuration file.")
    ] = EVG_CONFIG_FILE,
    build_events_file: Annotated[
        Optional[str],
        typer.Option(
            help="Path to the Bazel build events NDJSON file (build_events.json). "
            "When provided, asserts that every executed test has a corresponding "
            "Evergreen task, raising an error if any are missing."
        ),
    ] = None,
    verbose: Annotated[bool, typer.Option(help="Enable verbose logging.")] = False,
) -> None:
    """
    Activate the result task group for the current build variant and task.
    """
    enable_logging(verbose)

    expansions = read_yaml_file(expansion_file)
    build_variant = expansions.get("build_variant")
    task_name = expansions.get("task_name")
    version_id = expansions.get("version_id")

    if not all([build_variant, task_name, version_id]):
        LOGGER.error(
            "Missing required expansions",
            build_variant=build_variant,
            task_name=task_name,
            version_id=version_id,
        )
        return

    evg_api = RetryingEvergreenApi.get_api(config_file=evergreen_config, log_on_error=True)
    evg_api._http_retry = Retry(
        total=DEFAULT_HTTP_RETRY_ATTEMPTS + 10,
        backoff_factor=DEFAULT_HTTP_RETRY_BACKOFF_FACTOR,
        status_forcelist=DEFAULT_HTTP_RETRY_CODES,
        raise_on_status=False,
        raise_on_redirect=False,
    )

    activate_result_task_group(build_variant, task_name, version_id, evg_api, build_events_file)


if __name__ == "__main__":
    app()
