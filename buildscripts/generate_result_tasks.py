"""
Generate tasks for displaying bazel test results for all resmoke bazel tests.

This script creates a json config used in Evergreen's generate.tasks to add
tasks for displaying test results AND assigns them to appropriate variants upfront.

Queries bazel to determine which test targets will run on each variant based on
tag filters and other expansions, then generates both task definitions and
variant assignments in a single step.


Usage:
   bazel run //buildscripts:generate_result_tasks -- --outfile=generated_tasks.json

Options:
    --outfile           File path for the generated task config.
"""

import glob
import json
import os
import re
import shlex
import subprocess
import sys
from functools import cache
from typing import Optional

import runfiles
import typer
import yaml
from shrub.v2 import BuildVariant, FunctionCall, Task, TaskGroup
from shrub.v2.command import BuiltInCommand
from typing_extensions import Annotated

from buildscripts.ciconfig.evergreen import Task as EvergreenTask
from buildscripts.ciconfig.evergreen import Variant as EvergreenVariant
from buildscripts.ciconfig.evergreen import parse_evergreen_file
from buildscripts.util.read_config import read_config_file

RESMOKE_TEST_QUERY = 'attr(tags, "resmoke_suite_test", //...)'
RESMOKE_TESTS_TAG_FILTER = "resmoke_tests_tag_filter"
MASTER_PROJECT_NAME = "mongodb-mongo-master"
MASTER_PROJECT_CONFIG = "etc/evergreen.yml"
NIGHTLY_PROJECT_CONFIG = "etc/evergreen_nightly.yml"

app = typer.Typer(pretty_exceptions_show_locals=False)


def _bazel_binary() -> str:
    return os.environ.get("BAZEL_BINARY", "bazel")


def make_results_task(target: str) -> Task:
    commands = [
        FunctionCall("fetch remote test results", {"test_label": target}),
    ]

    task = Task(target, commands).as_dict()

    tag = get_assignment_tag(target)
    if tag:
        task["tags"] = [tag]

    return task


def make_task_group(
    name: str,
    variant: str,
    targets,
    resmoke_task: Optional[str] = "resmoke_tests",
) -> TaskGroup:
    task_group = TaskGroup(
        name=f"{name}_results_{variant}",
        tasks=[],
        max_hosts=len(targets),
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
                    + f"{resmoke_task}/build_events.json",
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
                    + f"bazel-invocation-{resmoke_task}-0.txt",
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
            FunctionCall("generate result task hang analyzer"),
        ],
        teardown_group=[
            FunctionCall("kill processes"),
            BuiltInCommand("shell.exec", {"script": "rm -rf build/ results/ report.json"}),
        ],
    )

    return task_group


def get_assignment_tag(target: str) -> Optional[str]:
    # Format is like "assigned_to_jira_team_devprod_build".
    # See also docs/evergreen-testing/yaml_configuration/task_ownership_tags.md

    assignment_tags = resolve_assignment_tags()
    tags = set()
    for codeowner in get_codeowners(target):
        if codeowner in assignment_tags:
            tags.add(assignment_tags[codeowner])
    if len(tags) > 1:
        print(
            f"Target {target} has {len(tags)} possible assignment tags based on it's codeowner: {tags}. Picking the first encountered.",
            file=sys.stderr,
        )
    return list(tags)[0] if tags else None


def get_codeowners(target: str) -> list[str]:
    package = target.split(":", 1)[0]
    return resolve_codeowners().get(package)


@cache
def resolve_assignment_tags() -> dict[str, str]:
    try:
        # Find the teams directory in the runfiles. Unfortunately, resolving the
        # directory requires resolving a specific file within the runfiles, so
        # an arbitrary team's YAML is used.
        r = runfiles.Create()
        teams_dir = os.path.dirname(r.Rlocation("mothra/mothra/teams/devprod.yaml"))

        teams = []
        for file in glob.glob(teams_dir + "/*.yaml"):
            with open(file, "rt") as f:
                teams += yaml.safe_load(f).get("teams", [])

        assignment_tags = {}
        for team in teams:
            evergreen_tag_name = team.get("evergreen_tag_name")
            github_teams = team.get("code_owners", {}).get("github_teams", [])
            for github_team in github_teams:
                name = github_team.get("team_name")
                if name and evergreen_tag_name:
                    assignment_tags[name] = "assigned_to_jira_team_" + evergreen_tag_name
        return assignment_tags
    except Exception as e:
        # Conservatively except any exception here. In the worst case, the contents/format of the
        # Mothra repo could change out from under us, and it should not completely fail
        # task generation.
        print(f"Failed to resolve assignment tags: {e}", file=sys.stderr)
        return {}


@cache
def resolve_codeowners() -> dict[str, list[str]]:
    try:
        result = subprocess.run(
            'find * -name "BUILD.bazel" | xargs bazel run --config=local @codeowners_binary//:codeowners --',
            shell=True,
            capture_output=True,
            text=True,
            check=True,
        )

        codeowners_map = {}
        for line in result.stdout.strip().split("\n"):
            if not line.strip():
                continue
            # Each line is formatted like: "./buildscripts/BUILD.bazel     @owner1 @owner2 ..."
            words = line.split()
            package = "//" + words[0].removeprefix("./").removesuffix("/BUILD.bazel")
            # Remove teams that don't provide a meaningful mapping to a real owner.
            owners = set(words[1:])
            owners.difference_update({"@svc-auto-approve-bot", "@10gen/mongo-default-approvers"})

            codeowners_map[package] = [owner.removeprefix("@") for owner in owners]
        return codeowners_map
    except subprocess.CalledProcessError as e:
        print(f"Failed to resolve codeowners: {e.returncode}", file=sys.stderr)
        print(f"STDOUT:\n{e.stdout}", file=sys.stderr)
        print(f"STDERR:\n{e.stderr}", file=sys.stderr)
        return {}


def expand_evergreen_variables(text: str, expansions: dict) -> str:
    """Expand Evergreen ${variable} syntax in a string.

    Args:
        text: String potentially containing ${var} expansions
        expansions: Dict of expansion values

    Returns:
        String with ${var} replaced by expansion values
    """

    def replace_var(match):
        var_name = match.group(1)
        return str(expansions.get(var_name, ""))

    return re.sub(r"\$\{([^}]+)\}", replace_var, text)


def get_task_vars(task: EvergreenTask, func_name: str = "execute resmoke tests via bazel") -> dict:
    """Extract vars from a specific function call in task commands."""
    for command in task.raw.get("commands", []):
        if command.get("func") == func_name:
            return command.get("vars", {})
    return {}


def get_variant_expansion(
    variant: EvergreenVariant, task: EvergreenTask, expansion_name: str
) -> str:
    """Get expansion value from variant or task vars.

    Checks variant expansions first, then task vars, then returns defaults.
    """
    task_vars = get_task_vars(task)
    value = task_vars.get(expansion_name)
    if value:
        return value

    value = variant.expansion(expansion_name)
    if value:
        return value

    return ""


def query_targets(
    variant,
    resmoke_task,
    expansions,
) -> list[str]:
    target_pattern = expansions.get("resmoke_test_targets", "//...")

    tag_filter = get_variant_expansion(variant, resmoke_task, RESMOKE_TESTS_TAG_FILTER)
    tags = [t.strip() for t in tag_filter.split(",") if t.strip()]
    if not tags:
        print(
            f"Warning: No tags found in filter '{tag_filter}' for variant {variant.name}",
            file=sys.stderr,
        )
        return []

    bazel_flags = []
    for flag_name in ["bazel_args", "bazel_compile_flags", "task_compile_flags"]:
        flag_value = get_variant_expansion(variant, resmoke_task, flag_name)
        if flag_value:
            flag_value = expand_evergreen_variables(flag_value, expansions)
            bazel_flags.extend(shlex.split(flag_value))

    flags_list = list(bazel_flags)
    flags_list.append("--//bazel/resmoke:skip_deps_for_cquery")
    flags_list.append("--noincompatible_enable_cc_toolchain_resolution")
    flags_list.append("--repo_env=no_c++_toolchain=1")
    flags_list.append("--keep_going")

    # If target_pattern contains multiple space-separated targets, wrap them in set()
    # to create valid Bazel query syntax
    if " " in target_pattern and not target_pattern.startswith("set("):
        target_pattern = f"set({target_pattern})"

    # Query for tests with tags that match the variant. Only py_test rules are considered,
    # since resmoke_suite_test is a macro for a py_test.
    excluded = f"attr(tags, '\\bincompatible_with_bazel_remote_test(?![a-zA-Z0-9_-])', kind('py_test', {target_pattern}))"
    if len(tags) == 1:
        # Single tag - simple query
        tag = tags[0]
        query = f"attr(tags, '\\b{tag}(?![a-zA-Z0-9_-])', kind('py_test', {target_pattern})) - {excluded}"
    else:
        # Multiple tags - use + operator to combine them in a single query
        tag_queries = [
            f"attr(tags, '\\b{tag}(?![a-zA-Z0-9_-])', kind('py_test', {target_pattern}))"
            for tag in tags
        ]
        query = f"({' + '.join(tag_queries)}) - {excluded}"

    cmd = (
        [_bazel_binary(), "cquery"]
        + flags_list
        + [
            query,
            "--output=starlark",
            "--starlark:expr",
            'target.label if "IncompatiblePlatformProvider" not in providers(target) else ""',
        ]
    )

    result = subprocess.run(cmd, capture_output=True, text=True)
    targets = [
        line.strip().removeprefix("@@")
        for line in result.stdout.strip().split("\n")
        if line.strip()
    ]
    print(f"Variant {variant.name}: Found {len(targets)} targets total", file=sys.stderr)

    if target_pattern == "//..." and not targets:
        error_msg = (
            f"Bazel cquery failed. No targets found for variant {variant.name}\n"
            f"Bazel cquery: {query}\n"
            f"Command: {' '.join(cmd)}\n"
            f"STDOUT:\n{result.stdout}\n"
            f"STDERR:\n{result.stderr}"
        )
        raise RuntimeError(error_msg)

    return targets


def create_task_group_for_variant(variant_name: str, task_name: str, targets: list[str]) -> dict:
    """Create task group definition for displaying test results.

    Structure is similar to append_result_tasks.py but generated upfront.
    """
    return {
        "name": f"{task_name}_results_{variant_name}",
        "tasks": targets,
        "max_hosts": len(targets),
        "setup_group_can_fail_task": True,
        "setup_group": [
            {"func": "git get project and add git tag"},
            {"func": "get engflow cert"},
            {"func": "get engflow key"},
            {
                "command": "s3.get",
                "params": {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "build_events.json",
                    "remote_file": f"${{project}}/${{version_id}}/${{build_variant}}/{task_name}/build_events.json",
                    "bucket": "mciuploads",
                },
            },
            {
                "command": "s3.get",
                "params": {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "resmoke-tests-bazel-invocation.txt",
                    "remote_file": f"${{project}}/${{build_variant}}/${{revision}}/bazel-invocation-{task_name}-0.txt",
                    "bucket": "mciuploads",
                },
            },
        ],
        "setup_task": [
            {"command": "shell.exec", "params": {"script": "rm -rf build/ results/ report.json"}}
        ],
        "teardown_task": [
            {"command": "attach.results", "params": {"file_location": "report.json"}},
            {
                "command": "s3.put",
                "params": {
                    "aws_key": "${aws_key}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "bazel-invocation.txt",
                    "remote_file": "${project}/${build_variant}/${revision}/bazel-invocation-${task_id}.txt",
                    "bucket": "mciuploads",
                    "permissions": "public-read",
                    "content_type": "text/plain",
                    "display_name": "Bazel invocation for local usage",
                },
            },
            {
                "command": "s3.put",
                "params": {
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
            },
            {
                "command": "s3.put",
                "params": {
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
            },
            {
                "command": "s3.put",
                "params": {
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
            },
            {"func": "generate result task hang analyzer"},
        ],
        "teardown_group": [
            {"func": "kill processes"},
            {"command": "shell.exec", "params": {"script": "rm -rf build/ results/ report.json"}},
        ],
    }


def get_evergreen_config_path(project_name: str) -> str:
    if project_name == MASTER_PROJECT_NAME:
        return MASTER_PROJECT_CONFIG
    return NIGHTLY_PROJECT_CONFIG


@app.command()
def main(outfile: Annotated[str, typer.Option()]):
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    expansions = read_config_file("../expansions.yml")
    project_name = expansions.get("project", MASTER_PROJECT_NAME)
    evg_config_path = get_evergreen_config_path(project_name)

    print(f"Parsing Evergreen configuration from {evg_config_path}...", file=sys.stderr)
    evg_config = parse_evergreen_file(evg_config_path)

    project = {"tasks": [], "task_groups": [], "buildvariants": []}

    targets_all = set()
    for variant in evg_config.variants:
        resmoke_task = variant.get_task("resmoke_tests")
        if not resmoke_task:
            continue

        targets = query_targets(variant, resmoke_task, expansions)
        if not targets:
            continue
        targets_all.update(targets)

        task_group = make_task_group("resmoke_tests", variant.name, targets).as_dict()
        task_group["tasks"] = targets
        project["task_groups"].append(task_group)

        build_variant = BuildVariant(name=variant.name).as_dict()
        # Typical variants running resmoke tests set a variant-wide dependency. During conversion,
        # these are not a dependency for the `resmoke_tests` task or the results tasks added here.
        # Set an explicitly depends_on in the task group's reference to override it.
        # The task that generated the task is used as a no-op dependency, as a workaround for not
        # being able to set an empty depends_on. Remove with SERVER-119809.
        build_variant["tasks"] = {
            "name": task_group["name"],
            "activate": False,
            "depends_on": {
                "name": "bazel_result_tasks_gen",
                "variant": "generate-tasks-for-version",
                "omit_generated_tasks": True,
            },
        }
        project["buildvariants"].append(build_variant)

        project["tasks"] = [make_results_task(target) for target in targets_all]

    with open(outfile, "w") as f:
        f.write(json.dumps(project, indent=4))


if __name__ == "__main__":
    app()
