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

# This file is for generating the task creates a docker manifest for the distro images produced via streams_build_and_publish.
# The docker manifest is used in order for the different architecture images to be pulled correctly without needing the particular architecture tag.


def make_testing_gate_task(compile_variant: str) -> Task:
    dependencies = {
        TaskDependency("aggregation", compile_variant.replace("-arm64", "")),
        TaskDependency("aggregation", compile_variant),
        TaskDependency(".streams_release_test", compile_variant.replace("-arm64", "")),
        TaskDependency(".streams_release_test", compile_variant),
    }

    return Task(f"streams_testing_gate_{compile_variant}", [], dependencies)


def make_task(compile_variant: str, break_glass: str) -> Task:
    task_name = "streams_publish_manifest_"
    dep_task_prefix = "streams_build_and_push_"
    script_args = ["./src/evergreen/streams_docker_manifest.sh"]

    dependencies = set()

    if break_glass == "true":
        task_name += "break_glass_"
        dep_task_prefix += "break_glass_"
        script_args.append("--break-glass")
    else:
        dependencies.add(TaskDependency("aggregation", compile_variant.replace("-arm64", "")))
        dependencies.add(TaskDependency("aggregation", compile_variant))
        dependencies.add(TaskDependency(".streams_release_test"))
    dependencies.add(TaskDependency(f"{dep_task_prefix}{compile_variant.replace('-arm64', '')}"))
    dependencies.add(TaskDependency(f"{dep_task_prefix}{compile_variant}"))

    commands = [
        BuiltInCommand("manifest.load", {}),
        FunctionCall("git get project and add git tag"),
        FunctionCall("f_expansions_write"),
        FunctionCall("set up venv"),
        FunctionCall(
            "set up remote credentials",
            {"aws_key_remote": "${repo_aws_key}", "aws_secret_remote": "${repo_aws_secret}"},
        ),
        BuiltInCommand(
            "ec2.assume_role", {"role_arn": "arn:aws:iam::664315256653:role/mongo-tf-project"}
        ),
        BuiltInCommand(
            "subprocess.exec",
            {
                "add_expansions_to_env": True,
                "binary": "bash",
                "args": script_args,
            },
        ),
    ]

    return Task(f"{task_name}{compile_variant}", commands, dependencies)


def main(
    expansions_file: Annotated[str, typer.Argument()] = "expansions.yml",
    output_file: Annotated[str, typer.Option("--output-file")] = "streams_publish_manifest.json",
    break_glass: Annotated[str, typer.Option("--break-glass")] = "false",
):
    expansions = read_config_file(expansions_file)
    distro = expansions.get("distro_id")
    build_variant_name = expansions.get("build_variant")
    current_task_name = expansions.get("task_name", "streams_publish_manifest_gen")

    compile_variant_name = expansions.get("compile_variant")
    if not compile_variant_name.endswith("-arm64"):
        raise RuntimeError("This task should only run on the arm64 compile variant")

    build_variant = BuildVariant(name=build_variant_name)
    build_variant.display_task(
        current_task_name.replace("_gen", ""),
        [
            make_task(compile_variant_name, break_glass=break_glass),
            make_testing_gate_task(compile_variant_name),
        ],
        distros=[distro],
    )
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file(output_file, shrub_project.json())


if __name__ == "__main__":
    typer.run(main)
