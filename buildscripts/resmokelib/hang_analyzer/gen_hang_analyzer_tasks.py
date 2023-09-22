#!/usr/bin/env python3
"""Generate a task to run core analysis on uploaded core dumps in evergreen."""
import argparse
import pathlib
import random
import string
import sys
from typing import Any, List, Set

from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskDependency, ExistingTask
from shrub.v2.command import BuiltInCommand

mongo_path = pathlib.Path(__file__).parents[3]
sys.path.append(mongo_path)

# pylint: disable=wrong-import-position
from buildscripts.util.fileops import write_file
from buildscripts.util.read_config import read_config_file
from buildscripts.resmokelib.utils import evergreen_conn


def get_core_analyzer_commands(task_id: str, execution: str) -> List[FunctionCall]:
    """Return setup commands."""
    return [
        FunctionCall("f_expansions_write"),
        BuiltInCommand("manifest.load", {}),
        FunctionCall("git get project and add git tag"),
        FunctionCall("f_expansions_write"),
        FunctionCall("kill processes"),
        FunctionCall("cleanup environment"),
        FunctionCall("set up venv"),
        FunctionCall("upload pip requirements"),
        FunctionCall("configure evergreen api credentials"),
        BuiltInCommand(
            "subprocess.exec", {
                "binary":
                    "bash", "args": [
                        "src/evergreen/run_python_script.sh",
                        "buildscripts/resmoke.py",
                        "core-analyzer",
                        f"--task-id={task_id}",
                        f"--execution={execution}",
                        "-o=file",
                        "-o=stdout",
                    ]
            }),
        BuiltInCommand(
            "archive.targz_pack", {
                "target": "src/mongo-coreanalysis.tgz",
                "source_dir": "src",
                "include": ["./core-analyzer/analysis/**"],
            }),
        BuiltInCommand(
            "s3.put", {
                "aws_key":
                    "${aws_key}",
                "aws_secret":
                    "${aws_secret}",
                "local_file":
                    "src/mongo-coreanalysis.tgz",
                "remote_file":
                    "${project}/${build_variant}/${revision}/hanganalyzer/mongo-coreanalysis-${build_id}-${task_name}-${execution}.tgz",
                "bucket":
                    "mciuploads",
                "permissions":
                    "public-read",
                "content_type":
                    "application/gzip",
                "display_name":
                    "Core Analyzer Output - Execution ${execution}",
            }),
        # We delete the core dumps after we are done processing them so they are not
        # reuploaded to s3 in the generated task's post task block
        FunctionCall(
            "remove files", {
                "files":
                    " ".join([
                        "src/core-analyzer/core-dumps/*.core", "src/core-analyzer/core-dumps/*.mdmp"
                    ])
            }),
    ]


def generate(expansions_file: str = "../expansions.yml",
             output_file: str = "hang_analyzer_task.json") -> None:

    if not sys.platform.startswith("linux"):
        print("This platform is not supported, skipping core analysis task generation.")
        return

    # gather information from the current task being run
    expansions = read_config_file(expansions_file)
    distro = expansions.get("distro_id")
    current_task_name = expansions.get("task_name")
    task_id = expansions.get("task_id")
    execution = expansions.get("execution")
    build_variant_name = expansions.get("build_variant")

    evg_api = evergreen_conn.get_evergreen_api()
    task_info = evg_api.task_by_id(task_id)
    generated_task_prefix = "core_analysis"

    # make sure we are not creating an infinite loop by generating a task from another generated task
    if current_task_name.startswith(generated_task_prefix):
        print(
            f"Skipping task generation because {current_task_name} starts with {generated_task_prefix}"
        )
        return

    # See if any core dumps were uploaded for this task
    has_core_dumps = any(artifact.name.startswith("Core Dump") for artifact in task_info.artifacts)

    if not has_core_dumps:
        print("No core dumps found for this task, skipping core analysis task generation.")
        return

    # The expansions do not unclude information for the parent display task
    # We have to query the evergreen api to get this information
    display_task_name = None
    if task_info.parent_task_id:
        display_task_name = evg_api.task_by_id(task_info.parent_task_id).display_name

    # Make the evergreen variant that will be generated
    build_variant = BuildVariant(name=build_variant_name, activate=True)
    commands = get_core_analyzer_commands(task_id, execution)

    # random string so we do not define the same task name for multiple variants which causes issues
    random_string = ''.join(
        random.choices(string.ascii_uppercase + string.digits + string.ascii_lowercase, k=5))
    sub_tasks = set(
        [Task(f"{generated_task_prefix}_{current_task_name}_{random_string}", commands)])

    if display_task_name:
        # If the task is already in a display task add the new task to the current display task
        build_variant.display_task(display_task_name, sub_tasks, distros=[distro])
    else:
        # If the task is not in a display task, just generate a new task
        build_variant.add_tasks(sub_tasks, distros=[distro])
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file(output_file, shrub_project.json())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--expansions-file", help="Location of evergreen expansions file.",
                        default="../expansions.yml")
    parser.add_argument("--output-file",
                        help="Name of output file to write the generated task config to.",
                        default="hang_analyzer_task.json")
    args = parser.parse_args()
    expansions_file = args.expansions_file
    output_file = args.output_file
    generate(expansions_file, output_file)
