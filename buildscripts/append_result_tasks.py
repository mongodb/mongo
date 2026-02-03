"""
Add generated tasks for displaying bazel test results to the current variant.

This script creates a json config used in Evergreen's generate.tasks to add
the appropriate subset of tasks to the variant, based on what tests were
reported to run in the build events JSON.

A task group is used to speed up successive tasks, and reduce the penalty of
setup costs.

See also: buildscripts/generate_result_tasks.py

Usage:
   bazel run //buildscripts:append_result_tasks -- --outfile=generated_tasks.json

Options:
    --outfile           File path for the generated task config.
    --build_events      Location of the build events JSON, default "./build_events.json".
"""

import json
import os
import re

import typer
from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, TaskGroup
from shrub.v2.command import BuiltInCommand
from shrub.v2.task import ExistingTask
from typing_extensions import Annotated

from buildscripts.bazel_burn_in import BAZEL_BURN_IN_TESTS
from buildscripts.gather_failed_tests import process_bep
from buildscripts.util.read_config import read_config_file

app = typer.Typer(pretty_exceptions_show_locals=False)


@app.command()
def main(outfile: Annotated[str, typer.Option()], build_events: str = "build_events.json"):
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    expansions = read_config_file("../expansions.yml")
    build_variant = expansions.get("build_variant")
    task_name = expansions.get("task_name")

    failed_tests, successful_tests = process_bep(build_events)
    tasks = failed_tests + successful_tests

    # Shrub's TaskGroup doesn't supporting adding existing tasks, so leave `tasks` empty and patch
    # the real list in later.
    task_group = TaskGroup(
        name=f"{task_name}_results_{build_variant}",
        tasks=[],
        max_hosts=len(tasks),
        setup_group_can_fail_task=True,
        setup_group=[
            FunctionCall("git get project and add git tag"),
            FunctionCall("get engflow cert"),
            FunctionCall("get engflow key"),
            BuiltInCommand(
                "s3.get",
                {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "build_events.json",
                    "remote_file": "${project}/${version_id}/${build_variant}/"
                    + f"{task_name}/build_events.json",
                    "bucket": "mciuploads",
                },
            ),
            BuiltInCommand(
                "s3.get",
                {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "resmoke-tests-bazel-invocation.txt",
                    "remote_file": "${project}/${build_variant}/${revision}/"
                    + f"bazel-invocation-{task_name}-0.txt",
                    "bucket": "mciuploads",
                },
            ),
        ],
        # Between tasks, remove the test logs and outputs. The tasks share hosts and leaving them
        # can cause the task to include test logs from other bazel targets.
        setup_task=[BuiltInCommand("shell.exec", {"script": "rm -rf build/ results/ report.json"})],
        teardown_task=[
            BuiltInCommand("attach.results", {"file_location": "report.json"}),
            BuiltInCommand(
                "s3.put",
                {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "bazel-invocation.txt",
                    "remote_file": "${project}/${build_variant}/${revision}/bazel-invocation-${task_id}.txt",
                    "bucket": "mciuploads",
                    "permissions": "public-read",
                    "content_type": "text/plain",
                    "display_name": "Bazel invocation for local usage",
                },
            ),
            BuiltInCommand(
                "s3.put",
                {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_files_include_filter_prefix": "results",
                    "local_files_include_filter": "**/*outputs.zip",
                    "remote_file": "${project}/${build_variant}/${revision}/${task_id}/",
                    "bucket": "mciuploads",
                    "permissions": "private",
                    "visibility": "signed",
                    "preserve_path": "true",
                    "content_type": "application/zip",
                },
            ),
            BuiltInCommand(
                "s3.put",
                {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_files_include_filter_prefix": "results",
                    "local_files_include_filter": "**/*_MANIFEST",
                    "remote_file": "${project}/${build_variant}/${revision}/${task_id}/",
                    "bucket": "mciuploads",
                    "permissions": "private",
                    "visibility": "signed",
                    "preserve_path": "true",
                    "content_type": "text/plain",
                },
            ),
            BuiltInCommand(
                "s3.put",
                {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_files_include_filter_prefix": "results",
                    "local_files_include_filter": "**/*test.log",
                    "remote_file": "${project}/${build_variant}/${revision}/${task_id}/",
                    "bucket": "mciuploads",
                    "permissions": "private",
                    "visibility": "signed",
                    "preserve_path": "true",
                    "content_type": "text/plain",
                },
            ),
        ],
        teardown_group=[
            FunctionCall("kill processes"),
            BuiltInCommand("shell.exec", {"script": "rm -rf build/ results/ report.json"}),
        ],
    )

    build_variant = BuildVariant(name=build_variant)
    if re.match(BAZEL_BURN_IN_TESTS, task_name):
        build_variant.display_task(
            display_name="burn_in_tests",
            execution_existing_tasks=[ExistingTask(task_name)]
            + [ExistingTask(task) for task in tasks],
        )

    build_variant.add_task_group(task_group)
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    # Patch in the real list of tasks in the task group.
    project = shrub_project.as_dict()
    project["task_groups"][0]["tasks"] = tasks

    with open(outfile, "w") as f:
        f.write(json.dumps(project, indent=4))


if __name__ == "__main__":
    app()
