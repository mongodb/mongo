#!/usr/bin/env python3
"""Command line utility to run smoke tests."""

import logging
import multiprocessing
import os.path
import shlex
import subprocess
import sys
from collections import defaultdict
from typing import Optional

import structlog
import typer
from structlog.stdlib import LoggerFactory
from typing_extensions import Annotated

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.resmokelib import parser, selector
from buildscripts.resmokelib.utils import globstar
from buildscripts.util.read_config import read_config_file

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)
DATA_DIRECTORY = "buildscripts/smoke_tests"
app = typer.Typer(add_completion=False)


def configure_logging(verbose: bool) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=level,
        stream=sys.stderr,
    )


def get_suite_path(suite_arg: str) -> str:
    implicit_path_ext = os.path.join(DATA_DIRECTORY, f"{suite_arg}")
    if os.path.isfile(implicit_path_ext):
        return implicit_path_ext

    implicit_path_noext = os.path.join(DATA_DIRECTORY, f"{suite_arg}.yml")
    if os.path.isfile(implicit_path_noext):
        return implicit_path_noext

    if os.path.isfile(suite_arg):
        return suite_arg

    raise RuntimeError(f"Could not find suite {suite_arg}")


def discover_suites(suite_names_or_paths: list[str] | None = None) -> list[str]:
    if suite_names_or_paths is None:
        return [
            suite_path
            for suite_path in globstar.iglob(os.path.join(DATA_DIRECTORY, "*.yml"))
            if "OWNERS.yml" not in suite_path
        ]
    return [get_suite_path(suite) for suite in suite_names_or_paths]


def map_suites_to_tests(config: dict) -> dict[str, list[str]]:
    try:
        suites = config["suites"]
    except KeyError:
        raise RuntimeError(f"The config file {config_file} is missing the suites key")

    suite_to_tests = defaultdict(set)

    for suite, patterns in suites.items():
        tests, _ = selector.filter_tests("js_test", {"roots": patterns})
        suite_to_tests[suite].update(tests)

    return suite_to_tests


def load_config_for_suites(suites: list[str]) -> dict[str, list[str]]:
    config = defaultdict(set)
    for suite in suites:
        suite_config_file = get_suite_path(suite)
        suite_config = read_config_file(suite_config_file)
        suite_to_tests = map_suites_to_tests(suite_config)
        for suite, tests in suite_to_tests.items():
            config[suite].update(tests)
    return config


def run_resmoke_suite(
    suite: str, tests: list[str], extra_args: list[str], *, dry_run: bool, jobs: int | None = None
) -> None:
    log = LOGGER.bind(suite=suite)

    resmoke_cmd = [
        sys.executable,
        "buildscripts/resmoke.py",
        "run",
        f"--suites={suite}",
        f"--jobs={jobs}" if jobs is not None else "",
        *tests,
        *extra_args,
    ]

    cmd_str = shlex.join(resmoke_cmd)
    if dry_run:
        log.info("Skipping resmoke invocation (dry-run)", cmd=cmd_str)
        return

    log.info("Running resmoke", cmd=cmd_str)
    try:
        subprocess.check_call(resmoke_cmd, shell=False)
    except subprocess.CalledProcessError as err:
        log.error(f"Resmoke returned an error with suite {suite}", cmd=shlex.join(resmoke_cmd))
        raise typer.Exit(code=err.returncode)

    log.info(f"All smoke tests in suite {suite} passed")


@app.command(context_settings={"allow_extra_args": True, "ignore_unknown_options": True})
def main(
    ctx: typer.Context,
    suites_str: Annotated[
        Optional[str],
        typer.Option("--suites", help="Comma-separated list of suites to run", metavar="suites"),
    ] = None,
    dry_run: Annotated[
        bool,
        typer.Option(
            "--dry-run", help="Do not execute the tests, just print the commands that would run."
        ),
    ] = False,
    verbose: Annotated[
        bool,
        typer.Option("--verbose", "-v"),
    ] = False,
    jobs: Annotated[
        int,
        typer.Option("--jobs", help="Number of jobs, passed through to resmoke if given."),
    ] = max(1, multiprocessing.cpu_count() / 2),
) -> None:
    """
    Run the given smoke test suite via a series of `resmoke.py run` commands.
    All arguments not interpreted by this script are passed through to resmoke, preceded by
    the --runAllFeatureFlagTests option, which this script always passes.
    Typical usage to run a "full" smoke test suite, including both C++ unit tests and jstests,
    involves a bazel or ninja command to run the C++ portion followed by a run of this script
    to execute the jstest portion.
    """

    configure_logging(verbose)

    default_resmoke_args = ["--runAllFeatureFlagTests"]
    passthrough_resmoke_args = default_resmoke_args + ctx.args
    parser.set_run_options(shlex.join(passthrough_resmoke_args))

    suite_names_or_paths = None if suites_str is None else suites_str.split(",")
    suites = discover_suites(suite_names_or_paths)
    LOGGER.debug("Discovered suites", suites=suites)
    suite_to_tests = load_config_for_suites(suites)

    for suite, tests in suite_to_tests.items():
        if len(tests) == 0:
            continue
        run_resmoke_suite(suite, tests, passthrough_resmoke_args, dry_run=dry_run, jobs=jobs)

    if not dry_run:
        LOGGER.info("All tests passed")


if __name__ == "__main__":
    app()
