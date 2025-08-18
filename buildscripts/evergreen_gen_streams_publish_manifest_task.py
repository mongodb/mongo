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

def make_task(compile_variant: str) -> Task:
    commands = [
        BuiltInCommand("manifest.load", {}),
        FunctionCall("git get project and add git tag"),
        FunctionCall("f_expansions_write"),
        FunctionCall("set up venv"),
        FunctionCall("set up remote credentials", {
            "aws_key_remote": "${repo_aws_key}",
            "aws_secret_remote": "${repo_aws_secret}"
        }),
        BuiltInCommand("ec2.assume_role", {"role_arn": "arn:aws:iam::664315256653:role/mongo-tf-project"}),
        BuiltInCommand("subprocess.exec", {
            "add_expansions_to_env": True,
            "binary": "bash",
            "args": ["./src/evergreen/streams_docker_manifest.sh"]
        }),
    ]
    dependencies = {
        TaskDependency(f"streams_build_and_publish_{compile_variant.replace('-arm64', '')}"),
        TaskDependency(f"streams_build_and_publish_{compile_variant}"),
    }
    return Task(f"streams_publish_manifest_{compile_variant}", commands, dependencies)


def main(
    expansions_file: Annotated[str, typer.Argument()] = "expansions.yml",
    output_file: Annotated[str, typer.Option("--output-file")] = "streams_publish_manifest.json",
):
    expansions = read_config_file(expansions_file)
    distro = expansions.get("distro_id")
    build_variant_name = expansions.get("build_variant")
    current_task_name = expansions.get("task_name", "streams_publish_manifest_gen")

    compile_variant_name = expansions.get("compile_variant")
    if (not compile_variant_name.endswith("-arm64")):
        raise RuntimeError("This task should only run on the arm64 compile variant")

    build_variant = BuildVariant(name=build_variant_name)
    build_variant.display_task(
        current_task_name.replace("_gen", ""),
        [make_task(compile_variant_name)],
        distros=[distro],
    )
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file(output_file, shrub_project.json())


if __name__ == "__main__":
    typer.run(main)
