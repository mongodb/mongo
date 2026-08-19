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
import traceback

import yaml

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

# Seconds to wait on the Evergreen select-tests endpoint before giving up and running everything.
# Matches the timeout resmoke itself uses in buildscripts/resmokelib/testing/suite.py.
_SELECT_TESTS_TIMEOUT = 60

# How much of an unexpected response body to quote when reporting a failure.
_ERROR_BODY_CHARS = 500

import requests
from oauthlib.oauth2 import BackendApplicationClient
from requests_oauthlib import OAuth2Session

from buildscripts.resmokelib import cli
from buildscripts.resmokelib import config as _config
from buildscripts.util.read_config import read_config_file

# Test selection is reached directly over the mesh rather than through the Evergreen REST API.
# Authentication is an Okta client-credentials token for a machine user, whose secret arrives in
# the task's expansions.
TSS_OAUTH_CLIENT_ID = "0oa12w49832J9h3bX298"
TSS_OAUTH_SCOPE = "kanopy"
OKTA_TOKEN_URL = "https://corp.mongodb.com/oauth2/aus4k4jv00hWjNnps297/v1/token"
# Name of both the environment variable and the Evergreen project variable holding the secret.
TSS_CLIENT_SECRET_KEY = "tss_mesh_client_secret"
# Evergreen patch parameter that opts a run into the staging deployment.
TSS_STAGING_PARAM = "use_tss_staging"
# Per-environment addresses of the service. The in-mesh name
# test-selection-services-webapp-web-app.cloud-build.svc.cluster.local would pick the matching
# environment by itself, but it does not resolve from an Evergreen host, so the environment has to
# be chosen here instead. See tss_environment().
TSS_BASE_URLS = {
    "staging": "https://test-selection-services.cloud-build.staging.corp.mongodb.com",
    "prod": "https://test-selection-services.cloud-build.prod.corp.mongodb.com",
}

# The service's StrategyEnum. Anything outside this set is rejected with a validation error, so
# check locally and say so rather than spending a round trip to find out.
TSS_STRATEGIES = (
    "ExcludeManuallyQuarantined",
    "Existential",
    "NotFailing",
    "NotPassing",
    "Optimistic",
    "StartsWithT",
)

# Test kinds whose "tests" are not file names, so there is nothing for selection to work with.
UNSELECTABLE_TEST_KINDS = ("mongos_test",)

# Selection endpoint on the TSS service. The trailing slash matters: without it the service
# answers with a redirect that adds it, which is a wasted round trip at best and, through the
# corp hostnames, pointed at a closed port.
TSS_SELECT_PATH = "/api/test_selection/select_tests/"


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


def find_workspace_root() -> pathlib.Path:
    """Return the real repository root.

    Bazel actions run from the output tree, so relative paths do not reach the checkout. The
    execroot does hold a MODULE.bazel symlink into the real repository, which
    bazel/resmoke/multiversion/run_db_contrib_tool.sh uses the same way to find the checkout.
    """
    return pathlib.Path(os.path.realpath("MODULE.bazel")).parent


def read_expansions() -> dict:
    """Return the task's expansions, or {} when not running in Evergreen.

    Evergreen writes expansions.yml next to the checkout, so it sits one level above the workspace
    root. Bazel scrubs the environment of its actions, so it has to be found by path rather than
    read from an env var.
    """
    try:
        expansions_file = find_workspace_root().parent / "expansions.yml"
        if not expansions_file.is_file():
            return {}
        return read_config_file(str(expansions_file)) or {}
    except OSError:
        return {}


def find_tss_client_secret() -> str:
    """Return the TSS machine user's client secret, or "" if it is not available.

    Prefers the environment, which is how a developer supplies the secret when running this
    directly. In Evergreen it comes from the task's expansions: f_expansions_write runs with
    'redacted: true', so private project variables are serialized into expansions.yml.
    """
    from_env = os.environ.get(TSS_CLIENT_SECRET_KEY)
    if from_env:
        return from_env
    return read_expansions().get(TSS_CLIENT_SECRET_KEY) or ""


def tss_session(client_secret: str) -> requests.Session:
    """Return a requests session that authenticates to TSS as the machine user.

    The token is fetched once and reused as a bearer header: OAuth2Session is not thread safe,
    and a plain session only ever reads the now-static access token.
    """
    oauth = OAuth2Session(client=BackendApplicationClient(client_id=TSS_OAUTH_CLIENT_ID))
    token = oauth.fetch_token(
        token_url=OKTA_TOKEN_URL,
        client_id=TSS_OAUTH_CLIENT_ID,
        client_secret=client_secret,
        scope=TSS_OAUTH_SCOPE,
    )
    session = requests.Session()
    session.headers.update({"Authorization": f"Bearer {token['access_token']}"})
    return session


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


def tss_environment() -> str:
    """Return which test selection deployment to talk to.

    Production, unless the run explicitly asks for staging. Nothing a task knows about itself
    names a deployment -- volatile-status describes the project, variant and build, and the
    generated src/.evergreen.yml always points at production Evergreen -- so staging has to be
    requested rather than detected:

        evergreen patch --param use_tss_staging=true ...
    """
    requested = str(read_expansions().get(TSS_STAGING_PARAM, "")).strip().lower()
    if requested in ("true", "1", "yes", "on"):
        return "staging"
    return "prod"


def select_tests(tests: list[str], label: str, volatile: dict, strategies: list[str]) -> list[str]:
    """Narrow the test list by asking test selection directly over the mesh.

    Returns the tests to run. Raises on any failure so the caller can fail open by keeping the
    full list -- test selection must only ever be able to make the list smaller, never to drop
    tests because it broke.
    """
    task_id = construct_task_id(volatile, label)
    if not task_id:
        raise RuntimeError(
            f"could not construct a task id from build_id {volatile.get('build_id')!r}"
        )

    base_url = TSS_BASE_URLS[tss_environment()]

    unknown = [strategy for strategy in strategies if strategy not in TSS_STRATEGIES]
    if unknown:
        raise RuntimeError(
            f"unknown test selection strategies {unknown}, expected any of "
            f"{list(TSS_STRATEGIES)}"
        )

    client_secret = find_tss_client_secret()
    if not client_secret:
        raise RuntimeError("no test selection client secret in the task's expansions")

    # Field names come from the service's Body_select_tests_with_data_in_body schema. Note
    # build_variant_name rather than build_variant, and test_names rather than tests.
    request = {
        "project_id": volatile.get("project", ""),
        "build_variant_name": volatile.get("build_variant", ""),
        "requester": volatile.get("requester", ""),
        "task_id": task_id,
        "task_name": label,
        # test_names is declared uniqueItems, so send each name once, in discovery order.
        "test_names": list(dict.fromkeys(tests)),
        "strategies": strategies,
    }
    response = tss_session(client_secret).post(
        f"{base_url}{TSS_SELECT_PATH}",
        json=request,
        timeout=_SELECT_TESTS_TIMEOUT,
        # Redirects here are never useful: the service's own slash redirect points at http:// on
        # a closed port, and an auth gateway redirects to a sign-in page. Following either buries
        # the cause in a timeout or an HTML body, so report the redirect itself instead.
        allow_redirects=False,
    )
    if response.is_redirect or response.is_permanent_redirect:
        raise RuntimeError(
            f"test selection returned {response.status_code} redirecting to "
            f"{response.headers.get('Location')!r} instead of answering. The request did not "
            "reach the endpoint."
        )
    if response.status_code >= 400:
        # Quote the body rather than using raise_for_status(): a validation error names the
        # fields it rejected, and that is the only thing that identifies a payload mismatch.
        raise RuntimeError(
            f"test selection returned {response.status_code}: "
            f"{response.text[:_ERROR_BODY_CHARS]!r}"
        )

    # The service answers with the chosen test names as a bare list, not wrapped in an object.
    # Report the status and body on anything unexpected: a 2xx that is not the list we expect
    # means the request reached something other than the endpoint we think it did, and the body
    # is what identifies it.
    try:
        result = response.json()
    except ValueError as exc:
        raise RuntimeError(
            f"test selection returned {response.status_code} with a non-JSON body "
            f"(content-type {response.headers.get('Content-Type')!r}): "
            f"{response.text[:_ERROR_BODY_CHARS]!r}"
        ) from exc
    if not isinstance(result, list):
        raise RuntimeError(
            f"test selection returned {response.status_code} with an unexpected shape, "
            f"expected a list of test names: {result!r}"
        )

    # Intersect rather than take the response as-is, so selection can only narrow the list. The
    # same guard exists in buildscripts/resmokelib/testing/suite.py.
    selected = set(result)
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
        print(
            f"Skipping test selection for {suite}: its tests are not files, so there is nothing "
            f"to select between. Wrote {args.output}.",
            file=sys.stderr,
        )
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
    selected = False
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
                selected = True
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

    # Report to stderr, which Bazel surfaces as "INFO: From Executing genrule ...", so the
    # outcome is visible in the build log without having to go find the output file.
    report(suite, args.output, tests, discovered, failed)
    if selected and len(tests) == discovered:
        print(
            f"Test selection ({tss_environment()}) did not narrow down any of the {discovered} "
            f"tests for {suite} using strategies {strategies}.",
            file=sys.stderr,
        )
    elif selected:
        print(
            f"Test selection ({tss_environment()}) narrowed {suite} from {discovered} to "
            f"{len(tests)} tests using strategies {strategies}.",
            file=sys.stderr,
        )


def report(suite: str, output: str, tests: list[str], discovered: int, failed: bool):
    """Print what was written, so the outcome is visible in the build log.

    'tests' is what ended up in the file, which is the selection when one was applied, so the
    discovered count is reported separately rather than inferred from it.
    """
    if failed:
        print(
            f"FAILED to discover tests for {suite}: wrote {output} with an empty list "
            "(failing open, see the traceback above). Test selection will not be able to "
            "narrow this suite.",
            file=sys.stderr,
        )
        return

    if not tests:
        print(
            f"WARNING: discovered 0 tests for {suite}, wrote {output}. This is not an error "
            "if the suite is genuinely empty for this configuration, but is worth checking.",
            file=sys.stderr,
        )
        return

    if len(tests) == discovered:
        print(f"Discovered {discovered} tests for {suite}, wrote {output}.", file=sys.stderr)
    else:
        print(
            f"Discovered {discovered} tests for {suite}, wrote the selected {len(tests)} "
            f"to {output}.",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
