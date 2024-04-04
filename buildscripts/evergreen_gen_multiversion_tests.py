#!/usr/bin/env python3
"""Generate multiversion tests to run in evergreen in parallel."""

import os
import re
import tempfile
from collections import defaultdict
from sys import platform

from typing import Optional
from subprocess import check_output
from github import GithubIntegration

import requests
import click
import structlog

from buildscripts.resmokelib.core.programs import get_path_env_var
from buildscripts.resmokelib.multiversionconstants import (
    LAST_LTS_MONGO_BINARY, LAST_CONTINUOUS_MONGO_BINARY, REQUIRES_FCV_TAG)
from buildscripts.resmokelib.utils import is_windows
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file
import buildscripts.ciconfig.tags as _tags
from buildscripts.util.read_config import read_config_file
# pylint: disable=len-as-condition

LOGGER = structlog.getLogger(__name__)

DEFAULT_CONFIG_DIR = "generated_resmoke_config"
CONFIG_DIR = DEFAULT_CONFIG_DIR

BACKPORT_REQUIRED_TAG = "backport_required_multiversion"
EXCLUDE_TAGS = f"{REQUIRES_FCV_TAG},multiversion_incompatible,{BACKPORT_REQUIRED_TAG}"
EXCLUDE_TAGS_FILE = "multiversion_exclude_tags.yml"

# The directory in which BACKPORTS_REQUIRED_FILE resides.
ETC_DIR = "etc"
BACKPORTS_REQUIRED_FILE = "backports_required_for_multiversion_tests.yml"
BACKPORTS_REQUIRED_BASE_URL = "https://raw.githubusercontent.com/10gen/mongo"


def get_backports_required_hash_for_shell_version(mongo_shell_path=None):
    """Parse the last-lts shell binary to get the commit hash."""
    env_vars = os.environ.copy()
    paths = get_path_env_var(env_vars=env_vars)
    env_vars["PATH"] = os.pathsep.join(paths)

    mongo_shell = mongo_shell_path
    if is_windows():
        mongo_shell = mongo_shell_path + ".exe"

    shell_version = check_output(f"{mongo_shell} --version", shell=True,
                                 env=env_vars).decode('utf-8')
    for line in shell_version.splitlines():
        if "gitVersion" in line:
            version_line = line.split(':')[1]
            # We identify the commit hash as the string enclosed by double quotation marks.
            result = re.search(r'"(.*?)"', version_line)
            if result:
                commit_hash = result.group().strip('"')
                if not commit_hash.isalnum():
                    raise ValueError(f"Error parsing commit hash. Expected an "
                                     f"alpha-numeric string but got: {commit_hash}")
                return commit_hash
            else:
                break
    raise ValueError("Could not find a valid commit hash from the last-lts mongo binary.")


def get_installation_access_token(app_id: int, private_key: str,
                                  installation_id: int) -> Optional[str]:  # noqa: D406,D407,D413
    """
    Obtain an installation access token using JWT.

    Args:
    - app_id: The application ID for GitHub App.
    - private_key: The private key associated with the GitHub App.
    - installation_id: The installation ID of the GitHub App for a particular account.

    Returns:
    - Optional[str]: The installation access token. Returns `None` if there's an error obtaining the token.
    """
    integration = GithubIntegration(app_id, private_key)
    auth = integration.get_access_token(installation_id)
    if auth:
        return auth.token
    else:
        LOGGER.info("Error obtaining installation token")
        return None


def get_last_lts_yaml(commit_hash, expansions_file):
    """Download BACKPORTS_REQUIRED_FILE from the last LTS commit and return the yaml."""
    LOGGER.info(f"Downloading file from commit hash of last-lts branch {commit_hash}")

    expansions = read_config_file(expansions_file)
    # Obtain installation access tokens using app credentials
    access_token_10gen_mongo = get_installation_access_token(
        expansions["app_id_10gen_mongo"], expansions["private_key_10gen_mongo"],
        expansions["installation_id_10gen_mongo"])

    response = requests.get(
        f'{BACKPORTS_REQUIRED_BASE_URL}/{commit_hash}/{ETC_DIR}/{BACKPORTS_REQUIRED_FILE}',
        headers={
            'Authorization': f'token {access_token_10gen_mongo}',
        })

    # If the response was successful, no exception will be raised.
    response.raise_for_status()

    last_lts_file = f"{commit_hash}_{BACKPORTS_REQUIRED_FILE}"
    temp_dir = tempfile.mkdtemp()
    with open(os.path.join(temp_dir, last_lts_file), "w") as fileh:
        fileh.write(response.text)

    backports_required_last_lts = read_yaml_file(os.path.join(temp_dir, last_lts_file))
    return backports_required_last_lts


@click.group()
def main():
    """Serve as an entry point for the 'run' and 'generate-exclude-tags' commands."""
    pass


@main.command("generate-exclude-tags")
@click.option("--output", type=str, default=os.path.join(CONFIG_DIR, EXCLUDE_TAGS_FILE),
              show_default=True, help="Where to output the generated tags.")
@click.option("--expansions-file", type=str, default="../expansions.yml", show_default=True,
              help="Location of evergreen expansions file.")
def generate_exclude_yaml(output: str, expansions_file: str) -> None:
    # pylint: disable=too-many-locals
    """
    Create a tag file associating multiversion tests to tags for exclusion.

    Compares the BACKPORTS_REQUIRED_FILE on the current branch with the same file on the
    last-lts branch to determine which tests should be denylisted.
    """

    enable_logging(False)

    location, _ = os.path.split(os.path.abspath(output))
    if not os.path.isdir(location):
        LOGGER.info(f"Cannot write to {output}. Not generating tag file.")
        return

    backports_required_latest = read_yaml_file(os.path.join(ETC_DIR, BACKPORTS_REQUIRED_FILE))

    # Get the state of the backports_required_for_multiversion_tests.yml file for the last-lts
    # binary we are running tests against. We do this by using the commit hash from the last-lts
    # mongo shell executable.
    last_lts_commit_hash = get_backports_required_hash_for_shell_version(
        mongo_shell_path=LAST_LTS_MONGO_BINARY)

    # Get the yaml contents from the last-lts commit.
    backports_required_last_lts = get_last_lts_yaml(last_lts_commit_hash, expansions_file)

    def diff(list1, list2):
        return [elem for elem in (list1 or []) if elem not in (list2 or [])]

    suites_latest = backports_required_latest["last-lts"]["suites"] or {}
    # Check if the changed syntax for etc/backports_required_for_multiversion_tests.yml has been
    # backported.
    # This variable and all branches where it's not set can be deleted after backporting the change.
    change_backported = "last-lts" in backports_required_last_lts.keys()
    if change_backported:
        always_exclude = diff(backports_required_latest["last-lts"]["all"],
                              backports_required_last_lts["last-lts"]["all"])
        suites_last_lts: defaultdict = defaultdict(
            list, backports_required_last_lts["last-lts"]["suites"])
    else:
        always_exclude = diff(backports_required_latest["last-lts"]["all"],
                              backports_required_last_lts["all"])
        suites_last_lts: defaultdict = defaultdict(list, backports_required_last_lts["suites"])

    tags = _tags.TagsConfig()

    # Tag tests that are excluded from every suite.
    for elem in always_exclude:
        tags.add_tag("js_test", elem["test_file"], BACKPORT_REQUIRED_TAG)

    # Tag tests that are excluded on a suite-by-suite basis.
    for suite in suites_latest.keys():
        test_set = set()
        for elem in diff(suites_latest[suite], suites_last_lts[suite]):
            test_set.add(elem["test_file"])
        for test in test_set:
            tags.add_tag("js_test", test, f"{suite}_{BACKPORT_REQUIRED_TAG}")

    LOGGER.info(f"Writing exclude tags to {output}.")
    tags.write_file(filename=output,
                    preamble="Tag file that specifies exclusions from multiversion suites.")


if __name__ == '__main__':
    main()  # pylint: disable=no-value-for-parameter
