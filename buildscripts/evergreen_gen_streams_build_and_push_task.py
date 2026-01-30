import os.path
import sys

import typer
from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskDependency
from shrub.v2.command import BuiltInCommand
from typing_extensions import Annotated

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.util.fileops import write_file
from buildscripts.util.read_config import read_config_file

# This file is for generating the task that builds and pushes the streams docker image.


def make_task(
    compile_variant: str,
    push: str,
    break_glass: str,
) -> Task:
    taskPrefix = "streams_build_"
    scriptArgs = ["./src/evergreen/streams_image_build_and_push.sh"]
    dependencies = {
        TaskDependency("archive_dist_test", compile_variant),
    }
    if push == "true":
        taskPrefix += "and_push_"
        scriptArgs.append("--push")
        if break_glass == "true":
            taskPrefix += "break_glass_"
            scriptArgs.append("--break-glass")

    commands = [
        BuiltInCommand("manifest.load", {}),
        FunctionCall("git get streams project and add git tag"),
        FunctionCall("f_expansions_write"),
        FunctionCall("set up venv"),
        FunctionCall("fetch binaries"),
        FunctionCall("extract binaries"),
    ]
    if push == "true":
        commands.append(
            FunctionCall(
                "set up remote credentials",
                {"aws_key_remote": "${repo_aws_key}", "aws_secret_remote": "${repo_aws_secret}"},
            )
        )
        commands.append(
            BuiltInCommand(
                "ec2.assume_role", {"role_arn": "arn:aws:iam::664315256653:role/mongo-tf-project"}
            )
        )
    commands.append(
        BuiltInCommand(
            "subprocess.exec",
            {
                "add_expansions_to_env": True,
                "binary": "bash",
                "args": scriptArgs,
            },
        )
    )

    return Task(f"{taskPrefix}{compile_variant}", commands, dependencies)


def main(
    expansions_file: Annotated[str, typer.Argument()] = "expansions.yml",
    output_file: Annotated[str, typer.Option("--output-file")] = "streams_build_and_push.json",
    push: Annotated[str, typer.Option("--push")] = "false",
    break_glass: Annotated[str, typer.Option("--break-glass")] = "false",
):
    expansions = read_config_file(expansions_file)
    build_variant_name = expansions.get("build_variant")

    distro = expansions.get("distro_id")
    compile_variant_name = expansions.get("compile_variant")
    current_task_name = expansions.get("task_name", "streams_build_and_push_gen")

    build_variant = BuildVariant(name=build_variant_name)
    build_variant.display_task(
        current_task_name.replace("_gen", ""),
        [
            make_task(
                compile_variant_name,
                push=push,
                break_glass=break_glass,
            )
        ],
        distros=[distro],
    )
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file(output_file, shrub_project.json())


if __name__ == "__main__":
    typer.run(main)
