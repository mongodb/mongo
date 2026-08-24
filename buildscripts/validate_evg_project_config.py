import logging
import os.path
import re
import subprocess
import sys
from collections import defaultdict

import structlog
import typer
import yaml
from typing_extensions import Annotated

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from buildscripts.ciconfig.evergreen import find_evergreen_binary

LOGGER = structlog.get_logger(__name__)

DEFAULT_EVG_PROJECT_NAME = "mongodb-mongo-master"
DEFAULT_EVG_NIGHTLY_PROJECT_NAME = "mongodb-mongo-master-nightly"
DEFAULT_EVG_PROJECT_CONFIG = "etc/evergreen.yml"
DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG = "etc/evergreen_nightly.yml"
# Quality-check callers use this distinct status to skip the Evergreen
# validation check when the local OAuth/device-auth flow is unavailable.
AUTHENTICATION_FAILURE_EXIT_CODE = 75

# SET TO TRUE IN RAPID RELEASE BRANCHES - see docs/branching/README.md
RELEASE_BRANCH = False

UNMATCHED_REGEXES = [
    re.compile(r".*buildvariant .+ has unmatched selector: .+"),
    re.compile(r".*buildvariant .+ has unmatched criteria: .+"),
]
ALLOWABLE_EVG_VALIDATE_MESSAGE_REGEXES = [
    # These regex match any number of repeated criteria that look like '.tag1 !.tag2'
    # unless they do not start with a dot or exclamation mark (meaning they are not
    # tag-based selectors)
    re.compile(r".*buildvariant .+ has unmatched selector: (('[!.][^']*?'),?\s?)+$"),
    re.compile(r".*buildvariant .+ has unmatched criteria: (('[!.][^']*?'),?\s?)+$"),
    re.compile(
        r".*task 'select_multiversion_binaries' defined but not used by any variants; consider using or disabling.*"
    ),  # this task is added to variants only alongside multiversion generated tasks
    re.compile(
        r".*depends on task 'archive_jstestshell' in build variant 'dsc-amazon2023-(arm64|x86)-compile', but it was not found.*"
    ),  # archive_jstestshell is produced by the compile_test_and_package_serial_TG task group
    # and is not visible to the static validator
]
ALLOWABLE_IF_NOT_IN_ALL_PROJECTS_EVG_VALIDATE_MESSAGE_REGEXES = [
    re.compile(r".*task .+ defined but not used by any variants; consider using or disabling.*"),
]

HORIZONTAL_LINE = "-" * 100


def messages_to_report(messages, num_of_projects):
    shared_evg_validate_messages = []
    error_on_evg_validate_messages = []
    for message in messages:
        if any(regex.match(message) for regex in ALLOWABLE_EVG_VALIDATE_MESSAGE_REGEXES):
            continue
        if num_of_projects > 1 and any(
            regex.match(message)
            for regex in ALLOWABLE_IF_NOT_IN_ALL_PROJECTS_EVG_VALIDATE_MESSAGE_REGEXES
        ):
            shared_evg_validate_messages.append(message)
            continue
        error_on_evg_validate_messages.append(message)
    return (error_on_evg_validate_messages, shared_evg_validate_messages)


def default_evg_config():
    config_locations = [
        os.path.join(os.getcwd(), ".evergreen.yml"),
        os.path.expanduser("~/.evergreen.yml"),
    ]
    for candidate in config_locations:
        if os.path.exists(candidate):
            return candidate
    LOGGER.error(f"No evergreen config exists at any of {config_locations}.")
    sys.exit(1)


def ensure_authenticated(evergreen_bin, evg_auth_config):
    """Log in before running any command whose output we capture.

    A missing or expired OAuth token otherwise turns `validate` into a silent five minute hang:
    it starts a device auth flow whose verification URI is swallowed by our output capture.

    Configs without an `oauth` section authenticate  with a static api key, so there is nothing to do here.
    """
    with open(evg_auth_config, encoding="utf-8") as auth_config_file:
        if not yaml.safe_load(auth_config_file).get("oauth"):
            return

    cmd = [evergreen_bin, "--config", evg_auth_config, "login"]
    LOGGER.info(f"Logging in to evergreen: {' '.join(cmd)}")
    if subprocess.run(cmd).returncode:
        sys.exit(AUTHENTICATION_FAILURE_EXIT_CODE)
    LOGGER.info("Logged in to evergreen.")


def main(
    evg_project_name: Annotated[
        str, typer.Option(help="Evergreen project name")
    ] = DEFAULT_EVG_PROJECT_NAME,
    evg_auth_config: Annotated[str, typer.Option(help="Evergreen auth config file")] = None,
    quiet: Annotated[
        bool, typer.Option(help="Only report errors and anything needing user interaction")
    ] = False,
):
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    if quiet:
        structlog.configure(
            wrapper_class=structlog.make_filtering_bound_logger(logging.WARNING),
        )

    if not evg_auth_config:
        evg_auth_config = default_evg_config()

    evg_project_config_map = {evg_project_name: DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG}
    if evg_project_name == DEFAULT_EVG_PROJECT_NAME:
        evg_project_config_map = {
            DEFAULT_EVG_NIGHTLY_PROJECT_NAME: DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG,
        }

    evergreen_bin = find_evergreen_binary("evergreen")

    ensure_authenticated(evergreen_bin, evg_auth_config)

    if RELEASE_BRANCH:
        for _, project_config in evg_project_config_map.items():
            cmd = [
                evergreen_bin,
                "--config",
                evg_auth_config,
                "evaluate",
                "--path",
                project_config,
            ]
            LOGGER.info(f"Running command: {cmd}")
            subprocess.run(cmd, capture_output=True, text=True, check=True)
            sys.exit(0)

    # The nightly config intentionally includes shared task definitions that are not
    # selected by any nightly variant. Validate it alongside the master config so
    # those warnings are only errors when a task is unused in both configurations.
    if evg_project_name in {DEFAULT_EVG_PROJECT_NAME, DEFAULT_EVG_NIGHTLY_PROJECT_NAME}:
        evg_project_config_map[DEFAULT_EVG_PROJECT_NAME] = DEFAULT_EVG_PROJECT_CONFIG

    shared_evg_validate_messages = []
    error_on_evg_validate_messages = defaultdict(list)

    exit_code = 0
    num_of_projects = len(evg_project_config_map)
    for project, project_config in evg_project_config_map.items():
        cmd = [
            evergreen_bin,
            "--config",
            evg_auth_config,
            "validate",
            "--project",
            project,
            "--path",
            project_config,
        ]
        LOGGER.info(f"Running command: {cmd}")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode:
            LOGGER.error(
                f"Command failed with return code {result.returncode}.\nstdout:{result.stdout}stderr:{result.stderr}"
            )
            exit_code = 1
        interesting_messages = result.stdout.strip().split("\n")[:-1]

        (error_on_evg_validate_messages[project], allowed_if_not_shared) = messages_to_report(
            interesting_messages, num_of_projects
        )
        shared_evg_validate_messages.extend(allowed_if_not_shared)

    error_on_shared_evg_validate_messages = []
    for message in set(shared_evg_validate_messages):
        if shared_evg_validate_messages.count(message) == num_of_projects:
            error_on_shared_evg_validate_messages.append(message)

    all_configs = list(evg_project_config_map.values())
    all_projects = list(evg_project_config_map.keys())

    for project, errors in error_on_evg_validate_messages.items():
        if len(errors) > 0:
            exit_code = 1
            project_config = evg_project_config_map[project]
            LOGGER.info(HORIZONTAL_LINE)
            LOGGER.error(f"Config '{project_config}' for '{project}' evergreen project has errors:")
            for error in errors:
                LOGGER.error(error)
                if any(regex.match(error) for regex in UNMATCHED_REGEXES):
                    LOGGER.info(
                        "Unmatched selector/criteria are allowed if they are tagged based (using '!' or '.'), but not if they directly name a task/task group"
                    )

    if len(error_on_shared_evg_validate_messages) > 0:
        exit_code = 1
        LOGGER.info(HORIZONTAL_LINE)
        LOGGER.error(
            f"Configs {all_configs} for evergreen projects {all_projects} have errors"
            f" (they can be fixed in either config):"
        )
        for error in error_on_shared_evg_validate_messages:
            LOGGER.error(error)

    if exit_code == 0:
        LOGGER.info(HORIZONTAL_LINE)
        LOGGER.info(
            f"Config(s) {all_configs} for evergreen project(s) {all_projects} is(are) valid"
        )

    sys.exit(exit_code)


app = typer.Typer(pretty_exceptions_show_locals=False)
app.command()(main)

if __name__ == "__main__":
    app()
