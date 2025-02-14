import os.path
import random
import shlex
import string
import sys

import typer
from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskDependency
from typing_extensions import Annotated

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import buildscripts.run_smoke_tests as smoke_tests
from buildscripts.util.fileops import write_file
from buildscripts.util.read_config import read_config_file

RANDOM_STRING_LENGTH = 5


def make_smoke_test_task(suite: str, tests: set[str], compile_variant: str) -> Task:
    resmoke_args = ["--runAllFeatureFlagTests"] + list(tests)
    commands = [
        FunctionCall("do setup"),
        FunctionCall("run tests", {"suite": suite, "resmoke_args": shlex.join(resmoke_args)}),
    ]
    dependencies = {TaskDependency("archive_dist_test", compile_variant)}
    # random string so we do not define the same task name for multiple variants which causes issues
    random_string = "".join(
        random.choices(
            string.ascii_uppercase + string.digits + string.ascii_lowercase, k=RANDOM_STRING_LENGTH
        )
    )
    return Task(f"smoke_tests_{suite}_{random_string}", commands, dependencies)


def main(
    expansions_file: Annotated[str, typer.Argument()] = "expansions.yml",
    output_file: Annotated[str, typer.Option("--output-file")] = "smoke_test_tasks.json",
):
    expansions = read_config_file(expansions_file)
    distro = expansions.get("distro_id")
    build_variant_name = expansions.get("build_variant")
    compile_variant_name = expansions.get("compile_variant")
    current_task_name = expansions.get("task_name", "run_smoke_tests_gen")

    suites_to_tests = smoke_tests.load_config_for_suites(smoke_tests.discover_suites())
    tasks = [
        make_smoke_test_task(suite, tests, compile_variant_name)
        for suite, tests in suites_to_tests.items()
    ]
    build_variant = BuildVariant(name=build_variant_name)
    build_variant.display_task(
        current_task_name.replace("_gen", ""),
        tasks,
        distros=[distro],
    )
    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file(output_file, shrub_project.json())


if __name__ == "__main__":
    typer.run(main)
