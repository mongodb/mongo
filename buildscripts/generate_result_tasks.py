"""
Generate tasks for displaying bazel test results for all resmoke bazel tests.

This script creates a json config used in Evergreen's generate.tasks to add
tasks for displaying test results. It does not add the tasks to any variant.
They are added to variants by the resmoke_tests task based on which tests ran.

See also: buildscripts/append_result_tasks.py

Usage:
   bazel run //buildscripts:generate_result_tasks -- --outfile=generated_tasks.json

Options:
    --outfile           File path for the generated task config.
"""

import json
import os
import subprocess
from typing import List

import typer
from shrub.v2 import FunctionCall, Task
from typing_extensions import Annotated

RESMOKE_TEST_QUERY = 'attr(tags, "resmoke_suite_test", //...)'

app = typer.Typer(pretty_exceptions_show_locals=False)


def make_task(target: str) -> Task:
    print(f"Generating task for {target}")
    commands = [
        FunctionCall("fetch remote test results", {"test_label": target}),
    ]

    return Task(target, commands)


def query_targets() -> List[str]:
    try:
        result = subprocess.run(
            ["bazel", "query", RESMOKE_TEST_QUERY],
            capture_output=True,
            text=True,
            check=True,
        )
        # Parse the output - each line is a target label
        return [line.strip() for line in result.stdout.strip().split("\n") if line.strip()]
    except subprocess.CalledProcessError as e:
        print(f"Bazel query failed with return code {e.returncode}")
        print(f"Command: {' '.join(e.cmd)}")
        print(f"STDOUT:\n{e.stdout}")
        print(f"STDERR:\n{e.stderr}")
        raise


@app.command()
def main(outfile: Annotated[str, typer.Option()]):
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    test_targets = query_targets()

    tasks = [make_task(target) for target in test_targets]
    project = {"tasks": [task.as_dict() for task in tasks]}

    with open(outfile, "w") as f:
        f.write(json.dumps(project, indent=4))


if __name__ == "__main__":
    app()
