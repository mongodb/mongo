#!/usr/bin/env python3

import argparse
import json
import subprocess
import uuid
from pathlib import Path

_MAX_RUNS = 1000


def print_pb_version_ids(run_id: str):
    result = subprocess.run(
        f"evergreen list-patches -j -n {_MAX_RUNS * 2}",
        shell=True,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    data = json.loads(result.stdout)

    filtered_objects = [
        obj for obj in data if obj.get("description") == f"flakinator run id: {run_id}"
    ]

    versions = []
    for obj in filtered_objects:
        versions += [obj["version"]]

    print("---")
    print(
        "For analysis, visit",
        f"https://ui.honeycomb.io/mongodb-4b/environments/production?query=%7B%22time_range%22%3A86400%2C%22granularity%22%3A0%2C%22breakdowns%22%3A%5B%22evergreen.task.name%22%5D%2C%22calculations%22%3A%5B%7B%22op%22%3A%22COUNT%22%7D%5D%2C%22filters%22%3A%5B%7B%22column%22%3A%22evergreen.task.name%22%2C%22op%22%3A%22!%3D%22%2C%22value%22%3A%22lint_repo%22%7D%2C%7B%22column%22%3A%22evergreen.project.id%22%2C%22op%22%3A%22%3D%22%2C%22value%22%3A%22mongodb-mongo-master%22%7D%2C%7B%22column%22%3A%22evergreen.task.status%22%2C%22op%22%3A%22%3D%22%2C%22value%22%3A%22failed%22%7D%2C%7B%22column%22%3A%22evergreen.version.id%22%2C%22op%22%3A%22in%22%2C%22value%22%3A%5B%22\
{'%22%2C%22'.join(versions)}\
%22%5D%7D%5D%2C%22filter_combination%22%3A%22AND%22%2C%22orders%22%3A%5B%7B%22op%22%3A%22COUNT%22%2C%22order%22%3A%22descending%22%7D%5D%2C%22havings%22%3A%5B%5D%2C%22limit%22%3A1000%7D",
    )
    print("---")


def commands_from_json_file(base_evergreen_args: str, json_file: Path):
    if json_file is None:
        return ""

    with json_file.open() as f:
        data = json.load(f)

    commands = []
    for variants_to_tasks in data["variants_to_tasks"]:
        variant = variants_to_tasks["_id"]
        tasks = ",".join(variants_to_tasks["tasks"])
        commands += [f"{base_evergreen_args} --tasks {tasks} --variants {variant}"]

    return commands


def deflakinator(run_id: str, runs: int, evergreen_args: str):
    """Run the deflakinator."""

    command = f'evergreen patch {evergreen_args} --yes --finalize --description "flakinator run id: {run_id}"'
    for _ in range(runs):
        print(command)
        try:
            subprocess.run(command, shell=True, check=True, text=True)
        except subprocess.CalledProcessError as e:
            print(e.stderr)
            break


def main():
    """Run the main function."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--runs",
        type=int,
        required=True,
        choices=range(1, _MAX_RUNS),
        metavar=f"[1-{_MAX_RUNS}]",
        help="Number of times to run each patch build",
    )
    parser.add_argument(
        "--evergreen-args",
        type=str,
        required=True,
        help='Arguments to pass to evergreen patch, example "--project mongodb-mongo-master --alias required"',
    )
    parser.add_argument(
        "--json-file",
        type=Path,
        help="Path to the JSON file containing additional variants and tasks to run. If not provided, --evergreen-args will be used alone.",
    )

    run_id = uuid.uuid4()

    print("Kicking off evergreen patch builds:")
    print("---")

    args = parser.parse_args()

    if args.json_file is not None:
        commands = commands_from_json_file(args.evergreen_args, args.json_file)
        for command in commands:
            deflakinator(run_id, args.runs, command)
    else:
        deflakinator(run_id, args.runs, args.evergreen_args)

    print_pb_version_ids(run_id)


if __name__ == "__main__":
    main()
