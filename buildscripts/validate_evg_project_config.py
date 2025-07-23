import os.path
import re
import subprocess
import sys
from collections import defaultdict

import structlog
import typer
from typing_extensions import Annotated

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from buildscripts.ciconfig.evergreen import find_evergreen_binary

LOGGER = structlog.get_logger(__name__)

DEFAULT_LOCAL_EVG_AUTH_CONFIG = os.path.expanduser("~/.evergreen.yml")

DEFAULT_EVG_PROJECT_NAME = "mongodb-mongo-master"
DEFAULT_EVG_NIGHTLY_PROJECT_NAME = "mongodb-mongo-master-nightly"
DEFAULT_EVG_PROJECT_CONFIG = "etc/evergreen.yml"
DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG = "etc/evergreen_nightly.yml"

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


def main(
    evg_project_name: Annotated[
        str, typer.Option(help="Evergreen project name")
    ] = DEFAULT_EVG_PROJECT_NAME,
    evg_auth_config: Annotated[
        str, typer.Option(help="Evergreen auth config file")
    ] = DEFAULT_LOCAL_EVG_AUTH_CONFIG,
):
    evg_project_config_map = {evg_project_name: DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG}
    if evg_project_name == DEFAULT_EVG_PROJECT_NAME:
        evg_project_config_map = {
            DEFAULT_EVG_NIGHTLY_PROJECT_NAME: DEFAULT_EVG_NIGHTLY_PROJECT_CONFIG,
        }
    
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

    if evg_project_name == DEFAULT_EVG_PROJECT_NAME:
        evg_project_config_map[DEFAULT_EVG_PROJECT_NAME] = DEFAULT_EVG_PROJECT_CONFIG

    shared_evg_validate_messages = []
    error_on_evg_validate_messages = defaultdict(list)

    num_of_projects = len(evg_project_config_map)
    evergreen_bin = find_evergreen_binary("evergreen")
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
        interesting_messages = result.stdout.strip().split("\n")[:-1]

        (error_on_evg_validate_messages[project], allowed_if_not_shared) = messages_to_report(
            interesting_messages, num_of_projects
        )
        shared_evg_validate_messages.extend(allowed_if_not_shared)

    error_on_shared_evg_validate_messages = []
    for message in set(shared_evg_validate_messages):
        if shared_evg_validate_messages.count(message) == num_of_projects:
            error_on_shared_evg_validate_messages.append(message)

    exit_code = 0
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


if __name__ == "__main__":
    typer.run(main)
