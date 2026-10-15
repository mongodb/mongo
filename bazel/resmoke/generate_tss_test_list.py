"""Writes the list of tests a resmoke suite will run.

Used as a genrule tool inside resmoke_suite_test to produce a per-suite test list. The list is
what Evergreen's Test Selection Services (TSS) needs to be asked about; nothing consumes this
file yet.

The list must match what the suite's test action would actually run, so discovery goes through
resmoke's own selector rather than reimplementing it, and is given the same --tagFile /
--excludeWithAnyTags arguments the test action gets.
"""

import argparse
import contextlib
import io
import os
import pathlib
import sys
import threading
import traceback

import yaml

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

# Seconds to wait on the Evergreen select-tests endpoint before giving up and running everything.
# Matches the timeout resmoke itself uses in buildscripts/resmokelib/testing/suite.py.
_SELECT_TESTS_TIMEOUT = 60

from buildscripts.resmokelib import cli
from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.utils import evergreen_conn

# Test kinds whose "tests" are not file names, so there is nothing for selection to work with.
UNSELECTABLE_TEST_KINDS = ("mongos_test",)


def is_selectable(suite_path: str) -> bool:
    """Return whether the suite's tests are named files that selection can reason about.

    A mongos_test suite has no test files: resmoke treats the selector config itself as the
    single test case (see Suite._get_tests_for_kind), so discovery has nothing to list and the
    service has nothing to choose between.
    """
    try:
        with open(suite_path) as suite_file:
            config = yaml.safe_load(suite_file)
    except (OSError, yaml.YAMLError):
        # Let discovery report the problem rather than second-guessing it here.
        return True
    if not isinstance(config, dict):
        # Valid YAML, but not a suite: a list or a scalar has no test_kind to inspect. Same
        # reasoning as an unreadable file -- discovery describes the problem far better than a
        # crash in this pre-check would, and crashing here would skip the fail-open handling
        # around discovery entirely and leave the genrule with no output file at all.
        return True
    return config.get("test_kind") not in UNSELECTABLE_TEST_KINDS


def discover_tests(suite: str, tag_files: list[str], exclude_with_any_tags: list[str]) -> list[str]:
    """Return the tests the suite will run, via resmoke's test-discovery subcommand.

    Deliberately does not pass --enableEvergreenApiTestSelection: Suite.tests runs
    _get_tests_for_kind(), which is where the TSS call itself lives, and asking TSS while
    building the list to ask TSS about would be circular.
    """
    argv = ["resmoke.py", "test-discovery", "--suite", suite]
    for tag_file in tag_files:
        argv += ["--tagFile", tag_file]
    for tags in exclude_with_any_tags:
        argv += ["--excludeWithAnyTags", tags]

    # test-discovery prints its YAML to stdout, so capture it. Run in-process rather than
    # shelling out so discovery keeps this action's interpreter and its resmoke runfiles;
    # a subprocess would have to reconstruct both.
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        cli.main(argv)

    tests = []
    for doc in yaml.safe_load_all(stdout.getvalue()):
        if doc:
            # gather_tests() has already flattened grouped tests.
            tests.extend(doc.get("tests", []))
    return tests


def parse_volatile_status(path: str) -> dict:
    """Return the key/value pairs Evergreen stamped into volatile-status, or {} if absent."""
    result = {}
    try:
        with open(path) as status:
            for line in status:
                parts = line.strip().split(" ", 1)
                if len(parts) == 2:
                    result[parts[0]] = parts[1]
    except OSError:
        pass
    return result


def normalize(value: str) -> str:
    """Apply Evergreen's id normalization to a name."""
    return value.replace("-", "_").replace("/", "_")


def construct_task_id(volatile: dict, task_name: str) -> str:
    """Return the Evergreen task id of the result task that will run this suite.

    Evergreen ids are <project>_<variant>_<task_name><rest>, where <rest> differs between
    mainline (_<revision>_<created>) and patches (_patch_<revision>_<version>_<created>). Rather
    than reassemble <rest>, splice the task name into build_id and copy the remainder verbatim,
    so the patch marker, revision, version id and timestamp all come from Evergreen itself.

    Returns "" if build_id does not have the expected shape, so callers can skip rather than
    send a fabricated id.
    """
    build_id = volatile.get("build_id", "")
    prefix = (
        f"{normalize(volatile.get('project', ''))}_{normalize(volatile.get('build_variant', ''))}"
    )
    if not build_id or not prefix or not build_id.startswith(prefix):
        return ""
    # Only "/" is normalized in the task name; Evergreen keeps the ":" of a bazel label.
    return f"{prefix}_{task_name.replace('/', '_')}{build_id[len(prefix):]}"


def task_id_format_is_understood(volatile: dict) -> bool:
    """Check our id construction against the one real task id we are given.

    volatile-status holds this (the runner) task's own id alongside the parts it was built from,
    so rebuilding it and comparing proves the format on every run instead of assuming it.
    """
    own_task_id = volatile.get("task_id", "")
    if not own_task_id:
        return False
    return construct_task_id(volatile, volatile.get("task_name", "")) == own_task_id


def read_selection_settings(path: str) -> tuple[bool, list[str]]:
    """Return (selection enabled, strategies) from the build settings file.

    The file holds the enable flag on the first line and the comma separated strategies on the
    second. An empty strategies line means the default, which is resmoke's own
    DEFAULT_EVERGREEN_TEST_SELECTION_STRATEGY, so the bazel path and the non-bazel path agree on
    both the input parameter and what it defaults to.
    """
    if not path:
        return False, []
    try:
        with open(path) as settings_file:
            lines = settings_file.read().splitlines()
    except OSError:
        return False, []

    enabled = bool(lines) and lines[0].strip().lower() == "true"
    strategies = [s.strip() for s in (lines[1] if len(lines) > 1 else "").split(",") if s.strip()]
    return enabled, strategies or list(_config.DEFAULT_EVERGREEN_TEST_SELECTION_STRATEGY)


def find_evergreen_config() -> str | None:
    """Return the checkout's .evergreen.yml, or None to use the usual lookup.

    Bazel actions run from the output tree, so the current directory and its parents do not
    reach the checkout, where Evergreen writes the file next to (or inside) the workspace. The
    execroot does hold a MODULE.bazel symlink into the real repository, which
    bazel/resmoke/multiversion/run_db_contrib_tool.sh uses the same way to find the checkout.
    """
    workspace_root = pathlib.Path(os.path.realpath("MODULE.bazel")).parent
    for directory in (workspace_root, workspace_root.parent):
        candidate = directory / ".evergreen.yml"
        if candidate.is_file():
            return str(candidate)
    return None


def select_tests(tests: list[str], label: str, volatile: dict, strategies: list[str]) -> list[str]:
    """Narrow the test list by asking Evergreen's select-tests REST endpoint.

    Returns the tests to run. Raises on any failure so the caller can fail open by keeping the
    full list -- test selection must only ever be able to make the list smaller, never to drop
    tests because it broke.
    """
    task_id = construct_task_id(volatile, label)
    if not task_id:
        raise RuntimeError(
            f"could not construct a task id from build_id {volatile.get('build_id')!r}"
        )

    evg_api = evergreen_conn.get_evergreen_api(find_evergreen_config())

    # Run the request on a daemon thread and wait for it with a deadline. A ThreadPoolExecutor
    # would not bound this: leaving its "with" block waits for the worker, so a stalled request
    # would hold up failing open. A daemon thread that outlives the deadline is simply abandoned
    # and does not keep the interpreter alive when main() returns.
    outcome: dict = {}

    def request():
        try:
            outcome["result"] = evg_api.select_tests(
                project_id=volatile.get("project", ""),
                build_variant=volatile.get("build_variant", ""),
                requester=volatile.get("requester", ""),
                task_id=task_id,
                task_name=label,
                tests=tests,
                strategies=strategies,
            )
        except BaseException as exc:
            outcome["error"] = exc

    worker = threading.Thread(target=request, daemon=True)
    worker.start()
    worker.join(_SELECT_TESTS_TIMEOUT)
    if worker.is_alive():
        raise TimeoutError(f"test selection did not answer within {_SELECT_TESTS_TIMEOUT}s")
    if "error" in outcome:
        raise outcome["error"]
    result = outcome["result"]

    if not isinstance(result, dict) or not isinstance(result.get("tests"), list):
        raise RuntimeError(f"test selection returned an unexpected response: {result!r}")

    # Intersect rather than take the response as-is, so selection can only narrow the list. The
    # same guard exists in buildscripts/resmokelib/testing/suite.py.
    selected = set(result["tests"])
    return [test for test in tests if test in selected]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", required=True, help="Path to the suite's resmoke YAML config.")
    parser.add_argument(
        "--label", help="Bazel label of the suite target, recorded in the output for debugging."
    )
    parser.add_argument("--output", required=True, help="File to write the test list to.")
    parser.add_argument(
        "--tagFile",
        dest="tag_files",
        action="append",
        default=[],
        help="Tag file to apply, as passed to the suite's test action. May be repeated.",
    )
    parser.add_argument(
        "--excludeWithAnyTags",
        dest="exclude_with_any_tags",
        action="append",
        default=[],
        help="Comma separated tags to exclude, as passed to the suite's test action.",
    )
    parser.add_argument(
        "--volatile-status",
        help="Path to volatile-status.txt, holding this build's Evergreen identity.",
    )
    parser.add_argument(
        "--selection-settings",
        help=(
            "File holding the test selection build settings: whether selection is enabled on the"
            " first line, and the comma separated strategies on the second. Without it, or with"
            " selection disabled, the discovered tests are recorded and the service is not called."
        ),
    )
    args = parser.parse_args()

    suite = args.label or args.suite

    if not is_selectable(args.suite):
        # The test suite's tests are not files, so there is nothing to select between.
        with open(args.output, "w") as output:
            yaml.safe_dump({"suite": suite, "status": "disabled", "tests": []}, output)
        return

    failed = False
    try:
        tests = discover_tests(args.suite, args.tag_files, args.exclude_with_any_tags)
    # SystemExit as well as Exception: resmoke's argument parsing exits rather than raising.
    except (Exception, SystemExit):
        # Fail open, matching how resmoke itself treats a failed TSS call
        # (buildscripts/resmokelib/testing/suite.py): an empty list means "we know nothing
        # about this suite", so a broken discovery step can never cause tests to be skipped.
        traceback.print_exc(file=sys.stderr)
        tests = []
        failed = True

    discovered = len(tests)
    # How the file came to hold what it holds. "failed" means something went wrong that a human
    # should look at, and resmoke fails the task on it; "disabled" is the ordinary state when
    # selection was not asked for, and is not an error.
    status = "failed" if failed else "disabled"
    enabled, strategies = read_selection_settings(args.selection_settings)
    if tests and enabled:
        volatile = parse_volatile_status(args.volatile_status) if args.volatile_status else {}
        if not volatile.get("task_id"):
            # No Evergreen identity, so this is a developer's machine rather than a CI host.
            print(
                f"Skipping test selection for {suite}: not running in Evergreen.",
                file=sys.stderr,
            )
        elif not task_id_format_is_understood(volatile):
            # Every id we would send is built the same way as the one that just failed to match,
            # so they would all be wrong. Skip rather than pollute the service with bad ids.
            print(
                "Skipping test selection: cannot reproduce this task's own id from its parts, so "
                "the Evergreen id format has changed and the ids we construct would be wrong. "
                f"Rebuilt {construct_task_id(volatile, volatile.get('task_name', ''))!r} from "
                f"build_id {volatile.get('build_id', '')!r}, but the real id is "
                f"{volatile.get('task_id', '')!r}.",
                file=sys.stderr,
            )
        else:
            try:
                tests = select_tests(tests, suite, volatile, strategies)
                status = "selected"
            except Exception:
                # Fail open in the other direction: keep every discovered test. Test selection
                # is an optimization, so a failure here must cost time, never coverage.
                traceback.print_exc(file=sys.stderr)
                status = "failed"
                print(
                    f"Test selection failed for {suite}, keeping all {discovered} tests.",
                    file=sys.stderr,
                )

    # "status" tells the reader how to treat the list. Without it an empty list would be
    # ambiguous: it could mean "selection excluded every test", "selection was never asked for" or
    # "discovery failed and we know nothing". Acting on the last of those would silently skip a
    # whole suite, and it is also the case resmoke turns into a task failure.
    with open(args.output, "w") as output:
        yaml.safe_dump({"suite": suite, "status": status, "tests": tests}, output)

    if failed:
        print(
            f"FAILED to discover tests for {suite}: wrote {args.output} with an empty list "
            "(failing open, see the traceback above). Test selection will not be able to "
            "narrow this suite.",
            file=sys.stderr,
        )
        return


if __name__ == "__main__":
    main()
