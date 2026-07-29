"""Activate result task groups in the existing build."""

import json
import os
import sys
from typing import Annotated, Optional

import structlog
import typer

from evergreen.api import EvergreenApi

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.generate_result_tasks import LOCAL_TASK_TAG
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.evergreen_retry import get_extra_retry_evergreen_api
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


def get_result_tasks(evg_api, build_id):
    tasks = []
    for task in evg_api.tasks_by_build(build_id):
        # Result tasks are bazel targets that start with "//"
        if task.display_name.startswith("//") and "_burn_in_" not in task.display_name:
            # Standalone local-exec tasks are activated early by the runner, not in the late RBE
            # activation; skip them here so they are not restarted.
            if LOCAL_TASK_TAG in (task.tags or []):
                continue
            tasks.append(task)
    return tasks


def get_local_tasks(evg_api, build_id):
    """The standalone local-exec tasks (tagged LOCAL_TASK_TAG) on a build."""
    return [
        task for task in evg_api.tasks_by_build(build_id) if LOCAL_TASK_TAG in (task.tags or [])
    ]


def activate_or_restart_tasks(evg_api, tasks, version_id, build_variant):
    activate = []
    for task in tasks:
        if task.activated:
            evg_api.restart_task(task.task_id)
        else:
            activate.append(task.display_name)

    if activate:
        variants = [{"name": build_variant, "tasks": activate}]
        evg_api.activate_version_tasks(version_id, variants)


def assert_all_tests_have_tasks(tasks, build_events_file):
    executed_labels = get_executed_test_labels(build_events_file)
    task_names = set([task.display_name for task in tasks])
    missing = executed_labels - task_names
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


def activate_result_task_group(
    build_variant: str,
    task_name: str,
    version_id: str,
    evg_api: EvergreenApi,
    build_events_file: Optional[str] = None,
    local_only: bool = False,
) -> None:
    """
    Activate result tasks for the given variant.

    :param build_variant: The build variant name.
    :param task_name: The task name (e.g., "resmoke_tests").
    :param version_id: The Evergreen version ID.
    :param evg_api: Evergreen API client.
    :param build_events_file: Optional path to build_events.json. When provided (RBE path), asserts
        that every executed test has a corresponding Evergreen task; raises RuntimeError otherwise.
    :param local_only: When True, activate only the standalone local-exec tasks (the runner calls
        this early so they run concurrently with remote execution). When False, activate the RBE
        result tasks (late, after the runner produces build_events.json).
    """

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

        if local_only:
            tasks = get_local_tasks(evg_api, build_id)
        else:
            tasks = get_result_tasks(evg_api, build_id)
            if build_events_file:
                assert_all_tests_have_tasks(tasks, build_events_file)

        activate_or_restart_tasks(evg_api, tasks, version_id, build_variant)

    except Exception:
        LOGGER.error(
            "Failed to activate result tasks",
            local_only=local_only,
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
    local_only: Annotated[
        bool,
        typer.Option(
            help="Activate only the standalone local-exec tasks (runner calls this early, before "
            "remote execution) instead of the RBE result tasks."
        ),
    ] = False,
    verbose: Annotated[bool, typer.Option(help="Enable verbose logging.")] = False,
) -> None:
    """
    Activate result tasks for the current build variant and task.
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

    evg_api = get_extra_retry_evergreen_api(evergreen_config)

    activate_result_task_group(
        build_variant, task_name, version_id, evg_api, build_events_file, local_only=local_only
    )


if __name__ == "__main__":
    app()
