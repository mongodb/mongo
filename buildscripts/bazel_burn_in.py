"""Burn-in generator for bazel-based resmoke test suites.

This module provides functionality to generate burn-in tests for changed test files
using Bazel targets. It identifies tests that have been modified in a git revision,
creates duplicate test targets with burn-in configurations (repeated execution), and
generates Evergreen task configurations to run these burn-in tests across build variants.

The two commands are:
generate-targets: generates burn-in test targets in BUILD.bazel files for changed tests
generate-tasks: generates Evergreen task configurations to execute burn-in tests

Usage:
    # First, generate resmoke configs:
    bazel build //... --build_tag_filters=resmoke_config
    bazel cquery "kind(resmoke_config, //...)" --output=starlark --starlark:expr "': '.join([str(target.label).replace('@@','')] + [f.path for f in target.files.to_list()])" > resmoke_suite_configs.yml

    # Generate burn-in test targets in BUILD.bazel files:
    python buildscripts/bazel_burn_in.py generate-targets <origin_rev>

    # Generate Evergreen tasks for burn-in tests:
    python buildscripts/bazel_burn_in.py generate-tasks <origin_rev> --outfile=generated_tasks.json
"""

import json
import os
import re
import subprocess
import sys
from functools import cache
from typing import List, NamedTuple

import typer
import yaml
from git import Repo
from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskGroup
from shrub.v2.command import BuiltInCommand
from typing_extensions import Annotated

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from buildscripts.burn_in_tests import (
    SELECTOR_FILE,
    SUPPORTED_TEST_KINDS,
    LocalFileChangeDetector,
)
from buildscripts.ciconfig.evergreen import parse_evergreen_file
from buildscripts.generate_result_tasks import make_results_task
from buildscripts.util import buildozer_utils as buildozer

BAZEL_BURN_IN_TESTS = r"resmoke_tests_burn_in_*"


def parse_bazel_target(target: str) -> tuple[str, str]:
    """
    Parse a bazel target to get the BUILD.bazel file path and target name.

    Args:
        target: Bazel target like "//buildscripts/resmokeconfig:core_config"

    Returns:
        Tuple of (build_file_path, target_name without _config suffix)
    """
    # Remove leading //
    target = target.removeprefix("//")

    # Split on :
    if ":" in target:
        package_path, target_name = target.split(":", 1)
    else:
        package_path = target
        target_name = target.split("/")[-1]

    # Remove _config suffix if present
    if target_name.endswith("_config"):
        target_name = target_name.removesuffix("_config")

    # Construct BUILD.bazel path
    package_parts = package_path.split("/")
    build_file_path = os.path.join(*package_parts, "BUILD.bazel")

    return build_file_path, target_name


def create_burn_in_target(target_original: str, target_burn_in: str, test: str):
    """ """
    # Create the label "//jstests:foo.js" from jstests/foo.js
    test_label = "//" + ":".join(test.rsplit("/", 1))

    build_file, name_original = parse_bazel_target(target_original)
    _, name_burn_in = parse_bazel_target(target_burn_in)

    # Buildozer does not provide a convenient way to clone an entire rule, so we print the original,
    # replace the "name" attribute, and then write it back to the BUILD.bazel.
    # To reduce the  likelihood of errors, all other edits are made using buildozer.
    rule_original = buildozer.bd_print([target_original], ["rule"])
    rule_new = re.sub(rf'(name\s*=\s*"){name_original}(")', rf"\1{name_burn_in}\2", rule_original)
    with open(build_file, "a") as f:
        f.write(rule_new)

    # Set the suite to only run the burn-in test, with only one shard.
    # All existing 'srcs' are kept as 'data', since it is common for jstests
    # to import each other.
    buildozer.bd_move([target_burn_in], "srcs", "data")
    buildozer.bd_set([target_burn_in], "srcs", test_label)
    buildozer.bd_set([target_burn_in], "shard_count", "1")

    # Add burn-in arguments to the suite to repeat the test
    resmoke_args_str = buildozer.bd_print([target_original], ["resmoke_args"])
    resmoke_args = resmoke_args_str.strip().removeprefix("[").removesuffix("]").split()

    # "(missing)" is buildozer's response if an attribute is not present
    if "(missing)" in resmoke_args:
        resmoke_args.remove("(missing)")
    resmoke_args.extend(
        [
            "--repeatTestsMax=1000",
            "--repeatTestsMin=2",
            "--repeatTestsSecs=600.0",
        ]
    )
    resmoke_args_str = "[" + ",".join(['"' + arg + '"' for arg in resmoke_args]) + "]"
    buildozer.bd_set([target_burn_in], "resmoke_args", resmoke_args_str)


class BurnInTargetInfo(NamedTuple):
    burn_in_target: str
    original_target: str
    test: str


def get_resmoke_configs():
    with open("resmoke_suite_configs.yml", "r") as f:
        return yaml.safe_load(f)


def query_targets_to_burn_in(origin_rev: str) -> List[BurnInTargetInfo]:
    change_detector = LocalFileChangeDetector(origin_rev)
    tests_changed = change_detector.find_changed_tests([Repo(".")])

    with open(SELECTOR_FILE, "r") as f:
        exclusions = yaml.safe_load(f)

    targets = []
    for config_label, config_path in get_resmoke_configs().items():
        test_label = config_label.removeprefix("@@").removesuffix("_config")
        with open(config_path, "r") as f:
            config = yaml.safe_load(f)

        test_kind = config["test_kind"]
        if test_kind not in SUPPORTED_TEST_KINDS:
            continue

        if test_label in exclusions["selector"].get(test_kind, {}).get("exclude_suites", []):
            continue

        for test in tests_changed:
            if test in exclusions["selector"].get(test_kind, {}).get("exclude_tests", []):
                continue
            if test not in config["selector"].get("roots"):
                continue

            burn_in_target = (
                test_label
                + "_burn_in_"
                + test.replace("/", "_").replace("\\", "_").removeprefix("_")
            )

            targets.append(
                BurnInTargetInfo(
                    burn_in_target=burn_in_target, original_target=test_label, test=test
                )
            )

    return targets


@cache
def get_targets_with_tag(tag: str) -> List[str]:
    try:
        query = f"attr(tags, '\\b{tag}(?![a-zA-Z0-9_-])', //...)"
        result = subprocess.run(
            ["bazel", "query", query],
            capture_output=True,
            text=True,
            check=True,
        )
        return [line.strip() for line in result.stdout.strip().split("\n") if line.strip()]
    except subprocess.CalledProcessError as e:
        print(f"Failed to query bazel targets with tag '{tag}': {e}")
        print(f"stdout: {e.stdout}")
        print(f"stderr: {e.stderr}")
        raise


def make_task(targets_to_run, variant_name):
    task = Task(
        name=f"resmoke_tests_burn_in_{variant_name}",
        commands=[
            FunctionCall(
                "execute resmoke tests via bazel",
                {
                    "targets": " ".join(targets_to_run),
                    "bazel_args": (
                        "--test_tag_filters=${resmoke_tests_tag_filter},-incompatible_with_bazel_remote_test "
                        "--test_arg=--testTimeout=960 "
                        "--test_timeout=1500 "
                        "--test_sharding_strategy=disabled "
                        "--test_arg=--sanityCheck"
                    ),
                    "task_compile_flags": (
                        "--keep_going "
                        "--verbose_failures "
                        "--simple_build_id=True "
                        "--define=MONGO_VERSION=${version} "
                        "--config=evg "
                        "--features=strip_debug "
                        "--separate_debug=False "
                        "--remote_download_outputs=minimal "
                        "--zip_undeclared_test_outputs"
                    ),
                    "generate_burn_in_targets": True,
                },
            ),
        ],
    )
    return TaskGroup(
        name=f"resmoke_tests_burn_in_{variant_name}-TG",
        tasks=[task],
        max_hosts=-1,
        setup_task=[
            BuiltInCommand("manifest.load", {}),
            FunctionCall("git get project and add git tag"),
            FunctionCall("set task expansion macros"),
            FunctionCall("f_expansions_write"),
            FunctionCall("kill processes"),
            FunctionCall("cleanup environment"),
            FunctionCall("set up venv"),
            FunctionCall("configure evergreen api credentials"),
            FunctionCall("set up credentials"),
            FunctionCall("get engflow creds"),
        ],
        teardown_task=[
            BuiltInCommand("generate.tasks", {"optional": True, "files": ["generated_tasks.json"]}),
            BuiltInCommand(
                "s3.put",
                {
                    "optional": True,
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "src/generated_tasks.json",
                    "remote_file": "${project}/${version_id}/${build_variant}/${task_name}/generated_tasks.json",
                    "bucket": "mciuploads",
                    "permissions": "private",
                    "visibility": "signed",
                    "content_type": "application/json",
                },
            ),
            BuiltInCommand(
                "s3.put",
                {
                    "optional": True,
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "src/build_events.json",
                    "remote_file": "${project}/${version_id}/${build_variant}/${task_name}/build_events.json",
                    "bucket": "mciuploads",
                    "permissions": "private",
                    "visibility": "signed",
                    "content_type": "application/json",
                },
            ),
            FunctionCall("debug full disk"),
            FunctionCall("attach bazel invocation"),
            FunctionCall("save failed tests"),
            FunctionCall("f_expansions_write"),
            FunctionCall("kill processes"),
        ],
        setup_group_can_fail_task=True,
    )


app = typer.Typer(pretty_exceptions_show_locals=False)


@app.command()
def generate_targets(origin_rev: str):
    """Generate burn-in test targets for changed test files."""
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    targets = query_targets_to_burn_in(origin_rev)
    print(f"\nFound {len(targets)} burn-in targets to generate\n")

    for burn_in_name, original_target, test in targets:
        print(f"Creating: {original_target} -> {burn_in_name}")

        create_burn_in_target(original_target, burn_in_name, test)


@app.command()
def generate_tasks(origin_rev: str, outfile: Annotated[str, typer.Option()]):
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    targets = query_targets_to_burn_in(origin_rev)

    evg_conf = parse_evergreen_file("etc/evergreen.yml")

    shrub_project = ShrubProject.empty()

    results_tasks = []
    for variant_name in evg_conf.variant_names:
        variant = evg_conf.get_variant(variant_name)
        if not (variant.is_required_variant() or variant.is_suggested_variant()):
            continue
        task = variant.get_task("resmoke_tests")
        if task:
            tags = variant.expansion("resmoke_tests_tag_filter").split(",")
            targets_with_tag = []
            for tag in tags:
                targets_with_tag += get_targets_with_tag(tag)

            burn_in_targets_to_run = [
                target.burn_in_target
                for target in targets
                if target.original_target in targets_with_tag
            ]
            if burn_in_targets_to_run:
                burn_in_task = make_task(burn_in_targets_to_run, variant_name)

                results_tasks.extend(
                    [make_results_task(target) for target in burn_in_targets_to_run]
                )

                build_variant = BuildVariant(name=variant_name)
                build_variant.add_task_group(burn_in_task)
                shrub_project.add_build_variant(build_variant)

    # Patch in fields that not supported by shrub
    project = shrub_project.as_dict()
    project["tasks"] = project.get("tasks", [])
    for variant in project.get("buildvariants", []):
        for task in variant.get("tasks", []):
            task["activate"] = False
    for task in project["tasks"]:
        task["exec_timeout_secs"] = 1800
    project["tasks"].extend([task.as_dict() for task in results_tasks])

    with open(outfile, "w") as f:
        f.write(json.dumps(project, indent=4))


if __name__ == "__main__":
    app()
