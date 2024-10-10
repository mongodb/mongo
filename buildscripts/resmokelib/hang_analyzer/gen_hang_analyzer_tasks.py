#!/usr/bin/env python3
"""Generate a task to run core analysis on uploaded core dumps in evergreen."""

import argparse
import json
import os
import pathlib
import random
import re
import string
import sys
from typing import List

from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskDependency
from shrub.v2.command import BuiltInCommand

from buildscripts.resmokelib.hang_analyzer import dumper

mongo_path = pathlib.Path(__file__).parents[3]
sys.path.append(mongo_path)

# pylint: disable=wrong-import-position
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.util.fileops import write_file
from buildscripts.util.read_config import read_config_file

GENERATED_TASK_PREFIX = "core_analysis"
RANDOM_STRING_LENGTH = 5
LOCAL_BIN_DIR = os.path.join("dist-test", "bin")
MULTIVERSION_BIN_DIR = os.path.normpath("/data/multiversion")


def get_generated_task_name(current_task_name: str, execution: str) -> str:
    # random string so we do not define the same task name for multiple variants which causes issues
    random_string = "".join(
        random.choices(
            string.ascii_uppercase + string.digits + string.ascii_lowercase, k=RANDOM_STRING_LENGTH
        )
    )
    return f"{GENERATED_TASK_PREFIX}_{current_task_name}{execution}_{random_string}"


def get_core_analyzer_commands(
    task_id: str, execution: str, core_analyzer_results_url: str, gdb_index_cache: str
) -> List[FunctionCall]:
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
            "subprocess.exec",
            {
                "binary": "bash",
                "args": [
                    "src/evergreen/run_python_script.sh",
                    "buildscripts/resmoke.py",
                    "core-analyzer",
                    f"--task-id={task_id}",
                    f"--execution={execution}",
                    f"--gdb-index-cache={gdb_index_cache}",
                    "--generate-report",
                ],
                "env": {
                    "OTEL_TRACE_ID": "${otel_trace_id}",
                    "OTEL_PARENT_ID": "${otel_parent_id}",
                    "OTEL_COLLECTOR_DIR": "../build/OTelTraces/",
                },
            },
        ),
        BuiltInCommand(
            "archive.targz_pack",
            {
                "target": "src/mongo-coreanalysis.tgz",
                "source_dir": "src",
                "include": ["./core-analyzer/analysis/**"],
            },
        ),
        BuiltInCommand(
            "s3.put",
            {
                "aws_key": "${aws_key}",
                "aws_secret": "${aws_secret}",
                "local_file": "src/mongo-coreanalysis.tgz",
                "remote_file": core_analyzer_results_url,
                "bucket": "mciuploads",
                "permissions": "public-read",
                "content_type": "application/gzip",
                "display_name": "Core Analyzer Output - Execution ${execution}",
            },
        ),
        # We delete the core dumps after we are done processing them so they are not
        # reuploaded to s3 in the generated task's post task block
        FunctionCall(
            "remove files",
            {
                "files": " ".join(
                    ["src/core-analyzer/core-dumps/*.core", "src/core-analyzer/core-dumps/*.mdmp"]
                )
            },
        ),
    ]


def generate(
    expansions_file: str = "../expansions.yml", output_file: str = "hang_analyzer_task.json"
) -> None:
    if not sys.platform.startswith("linux"):
        print("This platform is not supported, skipping core analysis task generation.")
        return

    # gather information from the current task being run
    expansions = read_config_file(expansions_file)
    distro = None
    for distro_expansion in ["core_analyzer_distro_name", "large_distro_name", "distro_id"]:
        if distro := expansions.get(distro_expansion, None):
            break
    assert distro is not None
    current_task_name = expansions.get("task_name")
    task_id = expansions.get("task_id")
    execution = expansions.get("execution")
    gdb_index_cache = "off" if expansions.get("core_analyzer_gdb_index_cache") == "off" else "on"
    build_variant_name = expansions.get("build_variant")
    core_analyzer_results_url = expansions.get("core_analyzer_results_url")
    compile_variant = expansions.get("compile_variant")

    try:
        evg_api = evergreen_conn.get_evergreen_api()
    except RuntimeError:
        print(
            "WARNING: Cannot generate core analysis because the evergreen api file could not be found.",
            file=sys.stderr,
        )
        print(
            "This is probably not an error, if you want core analysis to run on this task make sure",
            file=sys.stderr,
        )
        print(
            "the evergreen function 'configure evergreen api credentials' is called before this task",
            file=sys.stderr,
        )
        return

    task_info = evg_api.task_by_id(task_id)

    # make sure we are not creating an infinite loop by generating a task from another generated task
    if current_task_name.startswith(GENERATED_TASK_PREFIX):
        print(
            f"Skipping task generation because {current_task_name} starts with {GENERATED_TASK_PREFIX}"
        )
        return

    # LOCAL_BIN_DIR does not exists on non-resmoke tasks, so return early as there is no work to be done.
    if not os.path.exists(LOCAL_BIN_DIR):
        print(f"Skipping task generation because binary directory not found: {LOCAL_BIN_DIR}")
        return

    # See if any core dumps were uploaded for this task
    has_known_core_dumps = False
    dumpers = dumper.get_dumpers(None, None)

    for artifact in task_info.artifacts:
        regex = re.search(r"Core Dump [0-9]+ \((.*)\.gz\)", artifact.name)
        if not regex:
            continue

        core_file = regex.group(1)
        binary_name, bin_version = dumpers.dbg.get_binary_from_core_dump(core_file)
        dir_to_check = MULTIVERSION_BIN_DIR if bin_version else LOCAL_BIN_DIR
        binary_files = os.listdir(dir_to_check)
        if binary_name in binary_files:
            has_known_core_dumps = True
            break
        print(f"{core_file} was generated by {binary_name} but the binary was not found.")

    if not has_known_core_dumps:
        print(
            "No core dumps with known binaries found for this task, skipping core analysis task generation."
        )
        return

    # The expansions do not unclude information for the parent display task
    # We have to query the evergreen api to get this information
    display_task_name = None
    if task_info.parent_task_id:
        display_task_name = evg_api.task_by_id(task_info.parent_task_id).display_name

    # Make the evergreen variant that will be generated
    build_variant = BuildVariant(name=build_variant_name, activate=True)
    commands = get_core_analyzer_commands(
        task_id, execution, core_analyzer_results_url, gdb_index_cache
    )

    deps = {TaskDependency("archive_dist_test_debug", compile_variant)}
    # TODO SERVER-92571 add archive_jstestshell_debug dep for variants that have it.
    sub_tasks = set([Task(get_generated_task_name(current_task_name, execution), commands, deps)])

    if display_task_name:
        # If the task is already in a display task add the new task to the current display task
        build_variant.display_task(display_task_name, sub_tasks, distros=[distro])
    else:
        # If the task is not in a display task, just generate a new task
        build_variant.add_tasks(sub_tasks, distros=[distro])
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    # shrub.py currently does not support adding task deps that override the variant deps
    output_dict = shrub_project.as_dict()
    deps_list = []
    for dep in deps:
        deps_list.append(dep.as_dict())
    for variant in output_dict["buildvariants"]:
        for task in variant["tasks"]:
            task["depends_on"] = deps_list

    write_file(output_file, json.dumps(output_dict))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--expansions-file",
        help="Location of evergreen expansions file.",
        default="../expansions.yml",
    )
    parser.add_argument(
        "--output-file",
        help="Name of output file to write the generated task config to.",
        default="hang_analyzer_task.json",
    )
    args = parser.parse_args()
    expansions_file = args.expansions_file
    output_file = args.output_file
    generate(expansions_file, output_file)
