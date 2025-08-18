import os.path
import sys

import typer
from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskDependency
from shrub.v2.command import BuiltInCommand
from typing_extensions import Annotated

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.util.fileops import write_file
from buildscripts.util.read_config import read_config_file

# This file is for generating the task that builds and publishes the streams docker image.
# depends_on is only evaluated on task creation/validation, so all dependencies must exist prior to streams_build_and_publish.
# Streams currently depends on multiple generated test suite tasks, which is why this task must also be generated.

def make_task(compile_variant: str, additional_dependencies: set[str]) -> Task:
    commands = [
        BuiltInCommand("manifest.load", {}),
        FunctionCall("git get project and add git tag"),
        FunctionCall("f_expansions_write"),
        FunctionCall("set up venv"),
        FunctionCall("fetch binaries"),
        FunctionCall("extract binaries"),
        FunctionCall("set up remote credentials", {
            "aws_key_remote": "${repo_aws_key}",
            "aws_secret_remote": "${repo_aws_secret}"
        }),
        BuiltInCommand("ec2.assume_role", {"role_arn": "arn:aws:iam::664315256653:role/mongo-tf-project"}),
        BuiltInCommand("subprocess.exec", {
            "add_expansions_to_env": True,
            "binary": "bash",
            "args": ["./src/evergreen/streams_image_push.sh"]
        }),
    ]
    dependencies = {
        TaskDependency("archive_dist_test", compile_variant),
        TaskDependency("aggregation", compile_variant),
        TaskDependency(".streams_release_test"),
    }
    for dep in additional_dependencies:
        dependencies.add(TaskDependency(dep))
    return Task(f"streams_build_and_publish_{compile_variant}", commands, dependencies)

def main(
    expansions_file: Annotated[str, typer.Argument()] = "expansions.yml",
    output_file: Annotated[str, typer.Option("--output-file")] = "streams_build_and_publish.json",
):
    evg_api = evergreen_conn.get_evergreen_api()
    expansions = read_config_file(expansions_file)
    version_id = expansions.get("version_id")
    build_variant_name = expansions.get("build_variant")
    required_tasks = {"streams", "streams_kafka"}
    evg_version = evg_api.version_by_id(version_id)
    variant = evg_version.build_by_variant(build_variant_name)
    task_deps = []
    for task in variant.get_tasks():
        if task.display_name not in required_tasks:
            continue
        if task.execution_tasks:
            # is a display task
            for child_task_id in task.execution_tasks:
                child_task = evg_api.task_by_id(child_task_id)
                task_deps.append(child_task.display_name)
        else:
            # is not a display task
            task_deps.append(task.display_name)
        
        required_tasks.remove(task.display_name)
    
    print(task_deps)
    if required_tasks:
        print("The following required tasks were not found", required_tasks)
        raise RuntimeError("Could not find all required tasks")
    
    distro = expansions.get("distro_id")
    compile_variant_name = expansions.get("compile_variant")
    current_task_name = expansions.get("task_name", "streams_build_and_publish_gen")

    build_variant = BuildVariant(name=build_variant_name)
    build_variant.display_task(
        current_task_name.replace("_gen", ""),
        [make_task(compile_variant_name, additional_dependencies=task_deps)],
        distros=[distro],
    )
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file(output_file, shrub_project.json())


if __name__ == "__main__":
    typer.run(main)
