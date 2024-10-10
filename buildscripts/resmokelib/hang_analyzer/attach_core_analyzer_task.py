import argparse
import json
import os
import pathlib
import re
import sys
from typing import Optional

mongo_path = pathlib.Path(__file__).parents[3]
sys.path.append(mongo_path)

# pylint: disable=wrong-import-position
from buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks import (
    GENERATED_TASK_PREFIX,
    RANDOM_STRING_LENGTH,
)
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.util.read_config import read_config_file


def matches_generated_task_pattern(
    original_task_name: str, generated_task_name: str
) -> Optional[str]:
    regex = re.match(
        f"{GENERATED_TASK_PREFIX}_{original_task_name}([0-9]{{1,2}})_[a-zA-Z0-9]{{{RANDOM_STRING_LENGTH}}}",
        generated_task_name,
    )

    return regex.group(1) if regex else None


def maybe_attach_core_analyzer_task(
    expansions_file: str, conditional_file: str, artifact_output_file: str, results_output_file: str
):
    # This script runs for every task even if the task is passing.
    # This statement checks to see if the file that was made to generate a core analyzer task
    # lives on the machine or not.
    # This is a safe way to determine if a core analysis task was generated or not.
    if not os.path.exists(conditional_file):
        return

    evg_api = evergreen_conn.get_evergreen_api()
    expansions = read_config_file(expansions_file)
    current_task_id = expansions.get("task_id")
    current_task = evg_api.task_by_id(current_task_id)
    build_id = current_task.build_id
    current_task_name: str = current_task.display_name
    build = evg_api.build_by_id(build_id)

    # If the task is a part of a display task, search the parent's execution tasks
    # If the task has no parent search the whole build variant
    parent_id = current_task.parent_task_id
    search_tasks = evg_api.task_by_id(parent_id).execution_tasks if parent_id else build.tasks

    # The task id uses underscores instead of hyphens
    task_id_search_term = f"{GENERATED_TASK_PREFIX}_{current_task_name.replace('-', '_')}"

    matching_task = None
    matching_execution = None
    for task_id in search_tasks:
        if task_id_search_term not in task_id:
            continue

        task = evg_api.task_by_id(task_id)
        execution = matches_generated_task_pattern(current_task_name, task.display_name)

        if execution is not None:
            matching_task = task
            matching_execution = execution
            print(f"Found matching task id: {matching_task.task_id}")
            break

    if not matching_task:
        raise RuntimeError("No core analysis task found for this task")

    core_analysis_task_url = f"https://spruce.mongodb.com/task/{matching_task.task_id}"

    assert matching_task.generated_by == current_task_id

    # Check if the core analysis is from the current execution or a previous one
    gen_from_cur_execution = current_task.execution == int(matching_execution)

    artifact_name = (
        "Core Analyzer Task"
        if gen_from_cur_execution
        else f"Core Analyzer Task (Previous Execution #{matching_execution})"
    )

    core_analyzer_task_artifact = [
        {
            "name": artifact_name,
            "link": core_analysis_task_url,
            "visibility": "public",
        }
    ]

    with open(artifact_output_file, "w") as file:
        json.dump(core_analyzer_task_artifact, file, indent=4)

    # Do not link the task results if the analysis is from a previous execution.
    # We do not know how far the analysis has come and this temporary file could
    # overwrite the real analysis.
    if not gen_from_cur_execution:
        return

    file_lines = [
        "Core analysis is in progress.",
        "This file will be overwritten with the results when core analysis is finished.",
        "You can view core analysis progress at this evergreen task:",
        core_analysis_task_url,
    ]

    with open(results_output_file, "w") as file:
        file.write("\n".join(file_lines))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--expansions-file",
        "-e",
        help="Expansions file to read task info and aws credentials from.",
        default="../expansions.yml",
    )
    parser.add_argument(
        "--conditional-file",
        help="Path to file. If this file exists, that means task generation was successful.",
        default="hang_analyzer_task.json",
    )
    parser.add_argument(
        "--artifact-output-file",
        help="Name of output file to write artifact of the core analyzer task url to.",
        default="core_analyzer_artifact.json",
    )
    parser.add_argument(
        "--results-output-file",
        help="Name of output file to write the temperary core analyzer result text.",
        default="core_analyzer_results.txt",
    )
    args = parser.parse_args()
    expansions_file = args.expansions_file
    conditional_file = args.conditional_file
    artifact_output_file = args.artifact_output_file
    results_output_file = args.results_output_file
    maybe_attach_core_analyzer_task(
        expansions_file, conditional_file, artifact_output_file, results_output_file
    )
