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
from concurrent.futures import ThreadPoolExecutor
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

# Result tasks in a task group share a host. Remove the test logs and outputs between tasks, as
# leaving them can cause a task to report test logs from another bazel target, and remove the
# relinked binaries staged by gather_failed_tests so one task never uploads another's.
_RESULT_TASK_CLEANUP = (
    "rm -rf build/ results/ report.json src/dist-tests/ mongo-tests.tgz "
    "src/.failed_unittest_repro.txt src/.bazel_build_invocation src/.engflow_link"
)

app = typer.Typer(pretty_exceptions_show_locals=False)


def _bazel_binary() -> str:
    return os.environ.get("BAZEL_BINARY", "bazel")


# Override passed to local-exec result tasks so they select ONLY the incompatible suites at test
# time.
LOCAL_INCOMPATIBLE_FILTER = ",incompatible_with_bazel_remote_test"

# Evergreen task tag on the standalone local-exec tasks, so the resmoke_tests runner can activate
# them early (and so they are excluded from the late RBE result-task activation).
LOCAL_TASK_TAG = "resmoke_local_test"

# Local memory budget Bazel schedules test actions against, as a multiple of the host's physical
# RAM.
LOCAL_MEMORY_RAM_RATIO = "1.0"

# Bazel-target tags requesting that the task be scheduled on a bigger distro than the variant's
# default, if the test is tagged "incompatible_with_bazel_remote_test" and runs local.
REQUIRES_LARGE_HOST_TAG = "requires_large_host"
REQUIRES_XLARGE_HOST_TAG = "requires_xlarge_host"

_HOST_SIZE_TAGS = (
    (REQUIRES_XLARGE_HOST_TAG, "xlarge_distro_name"),
    (REQUIRES_LARGE_HOST_TAG, "large_distro_name"),
)


def query_target_tags(targets: list[str]) -> dict[str, list[str]]:
    """Return the bazel `tags` attribute for each target."""
    if not targets:
        return {}
    target_set = "set(" + " ".join(targets) + ")"
    result = subprocess.run(
        [_bazel_binary(), "query", "--keep_going", "--output=streamed_jsonproto", target_set],
        capture_output=True,
        text=True,
    )
    tags_by_target: dict[str, list[str]] = {}
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        rule = json.loads(line).get("rule", {})
        name = rule.get("name", "").removeprefix("@@").removeprefix("@")
        if not name:
            continue
        tags: list[str] = []
        for attr in rule.get("attribute", []):
            if attr.get("name") == "tags":
                tags = attr.get("stringListValue", [])
                break
        tags_by_target[name] = tags
    return tags_by_target


def make_results_task(
    target: str,
    resmoke_disable_rbe: bool = False,
    generate_burn_in_targets: bool = False,
) -> Task:
    if resmoke_disable_rbe:
        results_func = "gather local test results"
    else:
        results_func = "fetch remote test results"
    execute_params: dict = {"targets": target, "result_task": True}
    if resmoke_disable_rbe:
        execute_params["resmoke_disable_rbe"] = "true"
        execute_params["resmoke_bazel_test_incompatible_filter"] = LOCAL_INCOMPATIBLE_FILTER
    if generate_burn_in_targets:
        execute_params["generate_burn_in_targets"] = True
    commands = [
        FunctionCall("execute resmoke tests via bazel", execute_params),
        FunctionCall(results_func, {"test_label": target}),
    ]

    task = Task(target, commands).as_dict()

    tag = get_assignment_tag(target)
    if tag:
        task["tags"] = [tag]

    return task


def _result_artifact_uploads() -> list:
    uploads = [
        ("**/*outputs.zip", "application/zip"),
        ("**/*_MANIFEST", "text/plain"),
        ("**/*test.log", "text/plain"),
    ]
    return [
        BuiltInCommand(
            "s3.put",
            {
                "aws_key": "${aws_key_new}",
                "aws_secret": "${aws_secret}",
                "local_files_include_filter_prefix": "results",
                "local_files_include_filter": pattern,
                "remote_file": "${project}/${build_variant}/${revision}/${task_id}/",
                "bucket": "mciuploads",
                "permissions": "private",
                "visibility": "signed",
                "preserve_path": "true",
                "content_type": content_type,
            },
        )
        for pattern, content_type in uploads
    ]


def make_local_results_task(target: str) -> dict:
    """A standalone task that runs one incompatible_with_bazel_remote_test suite locally."""
    execute_params = {
        "targets": target,
        "result_task": True,
        "resmoke_disable_rbe": "true",
        "resmoke_bazel_test_incompatible_filter": LOCAL_INCOMPATIBLE_FILTER,
    }
    commands = (
        _make_setup_group("resmoke_tests", resmoke_disable_rbe=True)
        + [BuiltInCommand("shell.exec", {"script": "rm -rf build/ results/ report.json"})]
        + [
            FunctionCall("execute resmoke tests via bazel", execute_params),
            FunctionCall("gather local test results", {"test_label": target}),
        ]
        + _result_artifact_uploads()
    )
    task = Task(target, commands).as_dict()
    tags = [LOCAL_TASK_TAG]
    assignment = get_assignment_tag(target)
    if assignment:
        tags.append(assignment)
    task["tags"] = tags
    return task


def _make_setup_group(resmoke_task: str, resmoke_disable_rbe: bool) -> list:
    common = [
        FunctionCall("git get project and add git tag"),
        FunctionCall("set task expansion macros"),
        FunctionCall("f_expansions_write"),
        FunctionCall("set up venv"),
        FunctionCall("configure evergreen api credentials"),
        FunctionCall("set up credentials"),
        FunctionCall(
            "setup bazel (credentials, bazelrc)",
            {"bazel_local_memory_ram_ratio": LOCAL_MEMORY_RAM_RATIO}
            if resmoke_disable_rbe
            else None,
        ),
    ]
    if resmoke_disable_rbe:
        # Download and extract the pre-built dist-test binaries into src/ so that
        # //bazel/resmoke:installed_dist_test_enabled can glob dist-test/** from the workspace root.
        return common + [
            BuiltInCommand(
                "s3.get",
                {
                    "aws_key": "${aws_key_new}",
                    "aws_secret": "${aws_secret}",
                    "remote_file": "${mongo_binaries}",
                    "bucket": "mciuploads",
                    "local_file": "mongo-binaries.tgz",
                },
            ),
            BuiltInCommand(
                "shell.exec",
                {
                    "script": "tar -xf mongo-binaries.tgz -C src",
                },
            ),
        ]
    else:
        return common + [
            BuiltInCommand(
                "s3.get",
                {
                    "aws_key": "${aws_key_new}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "resmoke-tests-bazel-invocation.txt",
                    "remote_file": "${project}/${build_variant}/${revision}/"
                    + f"bazel-invocation-{resmoke_task}-0.txt",
                    "bucket": "mciuploads",
                    "optional": True,
                },
            ),
        ]


def _make_build_events_fetch(resmoke_task: str) -> list:
    """Per-task refresh of the runner's build_events.json.

    The runner uploads a growing snapshot of the BEP as each target finishes and activates that
    target's result task immediately after, so the snapshot at any point contains the events for
    every task activated so far. Refetching per task is what guarantees a task sees its own
    target's testResult events.
    """
    return [
        BuiltInCommand("shell.exec", {"script": "rm -f src/build_events.json"}),
        BuiltInCommand(
            "s3.get",
            {
                "aws_key": "${aws_key_new}",
                "aws_secret": "${aws_secret}",
                "local_file": "src/build_events.json",
                "remote_file": "${project}/${version_id}/${build_variant}/"
                + f"{resmoke_task}/build_events.json",
                "bucket": "mciuploads",
                "optional": True,
            },
        ),
    ]


def make_task_group(
    name: str,
    variant: str,
    targets,
    resmoke_task: Optional[str] = "resmoke_tests",
    resmoke_disable_rbe: bool = False,
) -> TaskGroup:
    task_group = TaskGroup(
        name=f"{name}_results_{variant}",
        tasks=[],
        max_hosts=len(targets),
        setup_group_can_fail_task=True,
        setup_group=_make_setup_group(resmoke_task, resmoke_disable_rbe),
        setup_task=[BuiltInCommand("shell.exec", {"script": _RESULT_TASK_CLEANUP})]
        + ([] if resmoke_disable_rbe else _make_build_events_fetch(resmoke_task)),
        teardown_task=[
            BuiltInCommand("attach.results", {"file_location": "report.json"}),
            BuiltInCommand(
                "s3.put",
                {
                    "aws_key": "${aws_key_new}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "bazel-invocation.txt",
                    "remote_file": "${project}/${build_variant}/${revision}/bazel-invocation-${task_id}.txt",
                    "bucket": "mciuploads",
                    "permissions": "public-read",
                    "content_type": "text/plain",
                    "display_name": "Bazel invocation for local usage",
                },
            ),
        ]
        + _result_artifact_uploads()
        + [
            FunctionCall("save failed tests"),
            FunctionCall("generate result task hang analyzer"),
        ],
        teardown_group=[
            FunctionCall("kill processes"),
            BuiltInCommand("shell.exec", {"script": _RESULT_TASK_CLEANUP}),
        ],
    )

    return task_group


def get_assignment_tag(target: str) -> Optional[str]:
    # Format is like "assigned_to_jira_team_devprod_build".
    # See also docs/evergreen-testing/yaml_configuration/task_ownership_tags.md

    # Route failures to devprod test infrastructure as RBE is rolled out. TODO SERVER-126116.
    return "assigned_to_jira_team_devprod_test_infrastructure"

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
    """Expand Evergreen ${variable} (and ${variable|default}) syntax in a string.

    Args:
        text: String potentially containing ${var} or ${var|default} expansions
        expansions: Dict of expansion values

    Returns:
        String with expansions resolved
    """

    def replace_var(match):
        name, sep, default = match.group(1).partition("|")
        value = expansions.get(name)
        if value is not None and value != "":
            return str(value)
        return default if sep else ""

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


def _build_tag_query(tags: list[str], target_pattern: str, local_exec: bool = False) -> str:
    positive_tags = [t for t in tags if not t.startswith("-")]
    negative_tags = [t[1:] for t in tags if t.startswith("-")]

    def tagged(tag: str) -> str:
        # resmoke_suite_test targets carrying `tag` as a whole word (the negative
        # lookahead prevents matching tag prefixes). The macro expands to the custom
        # _resmoke_test rule, and the ci-*/exclusion tags this query filters on are
        # only ever applied to resmoke suites, so no other rule kind is relevant.
        return (
            f"attr(tags, '\\b{tag}(?![a-zA-Z0-9_-])', " f"kind('_resmoke_test', {target_pattern}))"
        )

    if len(positive_tags) == 1:
        inclusion = tagged(positive_tags[0])
    else:
        inclusion = f"({' + '.join(tagged(tag) for tag in positive_tags)})"

    if local_exec:
        inclusion = f"{inclusion} ^ {tagged('incompatible_with_bazel_remote_test')}"
        excluded_parts = [tagged(tag) for tag in negative_tags]
    else:
        excluded_parts = [tagged("incompatible_with_bazel_remote_test")]
        excluded_parts += [tagged(tag) for tag in negative_tags]

    if excluded_parts:
        return f"{inclusion} - ({' + '.join(excluded_parts)})"
    return inclusion


def _variant_cquery_flags(variant, resmoke_task, expansions) -> tuple[list[str], list[str], str]:
    """Compute (tags, cquery_flags, target_pattern) for a variant."""
    target_pattern = expansions.get("resmoke_test_targets", "//...")

    tag_filter = get_variant_expansion(variant, resmoke_task, RESMOKE_TESTS_TAG_FILTER)
    tags = [t.strip() for t in tag_filter.split(",") if t.strip()]

    cquery_flags = []
    for flag_name in ["bazel_args", "bazel_compile_flags", "task_compile_flags"]:
        flag_value = get_variant_expansion(variant, resmoke_task, flag_name)
        if flag_value:
            flag_value = expand_evergreen_variables(flag_value, expansions)
            cquery_flags.extend(shlex.split(flag_value))

    cquery_flags.append("--//bazel/resmoke:skip_deps_for_cquery")
    cquery_flags.append("--repo_env=no_c++_toolchain=1")
    cquery_flags.append("--keep_going")
    # Suites with shard_count above Bazel's native cap of 50 get their shard count from a rule
    # transition, and Bazel omits a target whose own transition changed its configuration from
    # cquery output. Turning the flag off makes the transition a no-op so the targets are visible.
    cquery_flags.append("--//bazel/resmoke:support_extended_shard_count=False")

    if " " in target_pattern and not target_pattern.startswith("set("):
        target_pattern = f"set({target_pattern})"

    return tags, cquery_flags, target_pattern


def query_targets(
    variant,
    resmoke_task,
    expansions,
    resmoke_disable_rbe: bool = False,
) -> list[str]:
    tags, cquery_flags, target_pattern = _variant_cquery_flags(variant, resmoke_task, expansions)
    if not tags:
        print(f"Warning: No tag filter for variant {variant.name}", file=sys.stderr)
        return []

    # Phase 1: unconfigured `bazel query` for tag matching. This skips configured
    # analysis, which is what made running `bazel cquery` against //... slow.
    query_cmd = [
        _bazel_binary(),
        "query",
        "--keep_going",
        _build_tag_query(tags, target_pattern, local_exec=resmoke_disable_rbe),
    ]
    q_result = subprocess.run(query_cmd, capture_output=True, text=True)
    candidates = [line.strip() for line in q_result.stdout.strip().split("\n") if line.strip()]

    if not candidates:
        # An empty local-exec set is normal (a variant may have no remote incompatible suites). Only the RBE
        # set over //... is expected to be non-empty, so only that case is treated as an error.
        if target_pattern == "//..." and not resmoke_disable_rbe:
            error_msg = (
                f"Bazel query failed. No targets found for variant {variant.name}\n"
                f"Bazel query: {query_cmd[-1]}\n"
                f"Command: {' '.join(query_cmd)}\n"
                f"STDOUT:\n{q_result.stdout}\n"
                f"STDERR:\n{q_result.stderr}"
            )
            raise RuntimeError(error_msg)
        return []

    # Phase 2: configured `bazel cquery` scoped to the Phase 1 candidates, to
    # drop targets whose `target_compatible_with` excludes the variant's platform.
    candidate_set = "set(" + " ".join(candidates) + ")"
    cquery_cmd = (
        [_bazel_binary(), "cquery"]
        + cquery_flags
        + [
            candidate_set,
            "--output=starlark",
            "--starlark:expr",
            "str(target.label) + "
            '(" INCOMPATIBLE" if "IncompatiblePlatformProvider" in providers(target) else " OK")',
        ]
    )
    result = subprocess.run(cquery_cmd, capture_output=True, text=True)
    compatible = set()
    for line in result.stdout.splitlines():
        label, _, verdict = line.strip().rpartition(" ")
        if verdict == "OK":
            compatible.add(label.removeprefix("@@").removeprefix("@"))

    targets = [c for c in candidates if c in compatible]
    print(f"Variant {variant.name}: Found {len(targets)} targets total", file=sys.stderr)

    if target_pattern == "//..." and not targets and not resmoke_disable_rbe:
        error_msg = (
            f"Bazel cquery failed. No targets found for variant {variant.name}\n"
            f"Bazel cquery: {candidate_set}\n"
            f"Command: {' '.join(cquery_cmd)}\n"
            f"STDOUT:\n{result.stdout}\n"
            f"STDERR:\n{result.stderr}"
        )
        raise RuntimeError(error_msg)

    return targets


def resolve_large_host_distro(
    variant, target: str, tags_by_target: dict[str, list[str]]
) -> Optional[str]:
    """Return the larger distro a local target must run on for this variant, if any."""
    tags = tags_by_target.get(target, [])
    for tag, expansion_name in _HOST_SIZE_TAGS:
        if tag not in tags:
            continue
        distro_name = variant.expansion(expansion_name)
        if not distro_name:
            raise RuntimeError(
                f"Target {target} is tagged '{tag}' but variant "
                f"'{variant.name}' has no '{expansion_name}' expansion to schedule it on."
            )
        return distro_name
    return None


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
                    "aws_key": "${aws_key_new}",
                    "aws_secret": "${aws_secret}",
                    "local_file": "build_events.json",
                    "remote_file": f"${{project}}/${{version_id}}/${{build_variant}}/{task_name}/build_events.json",
                    "bucket": "mciuploads",
                },
            },
            {
                "command": "s3.get",
                "params": {
                    "aws_key": "${aws_key_new}",
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
                    "aws_key": "${aws_key_new}",
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
                    "aws_key": "${aws_key_new}",
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
                    "aws_key": "${aws_key_new}",
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
                    "aws_key": "${aws_key_new}",
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
    evg_config_path = expansions.get("evergreen_config_file_path") or get_evergreen_config_path(
        project_name
    )

    print(f"Parsing Evergreen configuration from {evg_config_path}...", file=sys.stderr)
    # Pre-warm the @cache-decorated resolvers so their bazel-run + YAML costs
    # overlap with parse_evergreen_file on the main thread.
    with ThreadPoolExecutor(max_workers=2) as bg_pool:
        bg_pool.submit(resolve_codeowners)
        bg_pool.submit(resolve_assignment_tags)

        evg_config = parse_evergreen_file(evg_config_path)

        project = {"tasks": [], "task_groups": [], "buildvariants": []}

        variant_tasks = []
        for variant in evg_config.variants:
            resmoke_task = variant.get_task("resmoke_tests")
            if not resmoke_task:
                continue
            variant_tasks.append((variant, resmoke_task))

        # Each variant runs BOTH its RBE-compatible suites (remotely, via the resmoke_tests runner)
        # and its incompatible_with_bazel_remote_test suites (locally on the host). The two sets are
        # disjoint and queried independently, then emitted as separate result-task groups.
        def query_both(vt):
            return (
                query_targets(vt[0], vt[1], expansions, resmoke_disable_rbe=False),
                query_targets(vt[0], vt[1], expansions, resmoke_disable_rbe=True),
            )

        with ThreadPoolExecutor(max_workers=max(2, len(variant_tasks))) as pool:
            targets_per_variant = list(pool.map(query_both, variant_tasks))

    # No-op dependency on the generating task, as a workaround for not being able to set an empty
    # depends_on (the variant-wide dependency must be overridden per task). Remove with SERVER-119809.
    gen_dep = {
        "name": "bazel_result_tasks_gen",
        "variant": "generate-tasks-for-version",
        "omit_generated_tasks": True,
    }

    # Fetch the bazel tags for every remote incompatible target in one query, so per-variant task refs
    # can be scheduled on the variant's large/xlarge distro when the suite is tagged
    # requires_large_host / requires_xlarge_host.
    local_targets_union: set[str] = set()
    for _, local_targets in targets_per_variant:
        local_targets_union.update(local_targets)
    local_target_tags = query_target_tags(sorted(local_targets_union))

    rbe_targets_all: set[str] = set()
    local_targets_all: set[str] = set()
    for (variant, _), (rbe_targets, local_targets) in zip(variant_tasks, targets_per_variant):
        task_refs = []

        # RBE-compatible suites: a task group whose tasks fetch results from the runner's batched
        # remote execution (activated late, after the runner produces build_events.json).
        if rbe_targets:
            rbe_targets_all.update(rbe_targets)
            task_group = make_task_group(
                "resmoke_tests", variant.name, rbe_targets, resmoke_disable_rbe=False
            ).as_dict()
            task_group["tasks"] = rbe_targets
            project["task_groups"].append(task_group)
            task_refs.append(
                {"name": task_group["name"], "activate": False, "depends_on": [gen_dep]}
            )

        # incompatible_with_bazel_remote_test suites: standalone tasks, each runs its suite locally.
        # The resmoke_tests runner activates them early (see resmoke_tests_execute_bazel.sh). They
        # depend on archive_dist_test for the dist-test binaries.
        if local_targets:
            local_targets_all.update(local_targets)
            compile_variant = variant.expansion("compile_variant") or variant.name
            local_dep = {"name": "archive_dist_test", "variant": compile_variant}
            for target in local_targets:
                task_ref = {"name": target, "activate": False, "depends_on": [gen_dep, local_dep]}
                # A suite tagged requires_large_host / requires_xlarge_host runs on this variant's
                # large / xlarge distro. Set run_on on
                # the task ref (not the shared standalone task) so each variant gets its own distro.
                large_distro_name = resolve_large_host_distro(variant, target, local_target_tags)
                if large_distro_name:
                    task_ref["run_on"] = [large_distro_name]
                task_refs.append(task_ref)

        if task_refs:
            build_variant = BuildVariant(name=variant.name).as_dict()
            build_variant["tasks"] = task_refs
            project["buildvariants"].append(build_variant)

    project["tasks"] = [
        make_results_task(target, resmoke_disable_rbe=False) for target in rbe_targets_all
    ] + [make_local_results_task(target) for target in local_targets_all]

    with open(outfile, "w") as f:
        f.write(json.dumps(project, indent=4))


if __name__ == "__main__":
    app()
