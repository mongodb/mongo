"""Generate multiversion exclude tags file."""

import logging
import os
import re
import subprocess
import tempfile
from collections import defaultdict
from subprocess import check_output

from buildscripts.resmokelib.config import MultiversionOptions
from buildscripts.resmokelib.core.programs import get_path_env_var
from buildscripts.resmokelib.testing import tags as _tags
from buildscripts.resmokelib.utils import is_windows
from buildscripts.util.fileops import read_yaml_file

BACKPORT_REQUIRED_TAG = "backport_required_multiversion"

# The directory in which BACKPORTS_REQUIRED_FILE resides.
ETC_DIR = "etc"
BACKPORTS_REQUIRED_FILE = "backports_required_for_multiversion_tests.yml"
BACKPORTS_REQUIRED_BASE_URL = "https://raw.githubusercontent.com/10gen/mongo"


def get_backports_required_hash_for_shell_version(mongo_shell_path: str | None = None):
    """Parse the old shell binary to get the commit hash."""
    env_vars = os.environ.copy()
    paths = get_path_env_var(env_vars=env_vars)
    env_vars["PATH"] = os.pathsep.join(paths)

    mongo_shell = mongo_shell_path
    if is_windows():
        mongo_shell = mongo_shell_path + ".exe"

    shell_version = check_output(f"{mongo_shell} --version", shell=True, env=env_vars).decode(
        "utf-8"
    )
    for line in shell_version.splitlines():
        if "gitVersion" in line:
            version_line = line.split(":")[1]
            # We identify the commit hash as the string enclosed by double quotation marks.
            result = re.search(r'"(.*?)"', version_line)
            if result:
                commit_hash = result.group().strip('"')
                if not commit_hash.isalnum():
                    raise ValueError(
                        f"Error parsing commit hash. Expected an "
                        f"alpha-numeric string but got: {commit_hash}"
                    )
                return commit_hash
            else:
                break
    raise ValueError(
        f"Could not find a valid commit hash from the {mongo_shell_path} mongo binary."
    )


def get_git_file_content(commit_hash: str) -> str:
    """Retrieve the content of a file from a specific commit in a local Git repository."""

    git_command = ["git", "show", f"{commit_hash}:{ETC_DIR}/{BACKPORTS_REQUIRED_FILE}"]
    git_fetch_command = ["git", "fetch", "--depth", "1", commit_hash]

    try:
        result = subprocess.run(git_command, capture_output=True, text=True, check=True)
        return result.stdout
    except subprocess.CalledProcessError:
        try:
            # If the git show command failed once, we attempt to shallow fetch the commit
            # to ensure we have the commit's contents then try again.
            _ = subprocess.run(git_fetch_command, capture_output=True, text=True, check=True)
            result = subprocess.run(git_command, capture_output=True, text=True, check=True)
        except subprocess.CalledProcessError as err:
            raise RuntimeError(
                f"Failed to retrieve file content using command: {' '.join(git_command)}. Error: {err.stderr}"
            )


def get_old_yaml(commit_hash: str):
    """Download BACKPORTS_REQUIRED_FILE from the old commit and return the yaml."""

    file_content = get_git_file_content(commit_hash)

    old_yaml_file = f"{commit_hash}_{BACKPORTS_REQUIRED_FILE}"
    temp_dir = tempfile.mkdtemp()
    temp_old_yaml_file = os.path.join(temp_dir, old_yaml_file)

    with open(temp_old_yaml_file, "w") as fileh:
        fileh.write(file_content)

    backports_required_old = read_yaml_file(temp_old_yaml_file)
    return backports_required_old


def generate_exclude_yaml(old_bin_version: str, output: str, logger: logging.Logger) -> None:
    """
    Create a tag file associating multiversion tests to tags for exclusion.

    Compares the BACKPORTS_REQUIRED_FILE on the current branch with the same file on the
    last-lts and/or last-continuous branch to determine which tests should be denylisted.
    """

    output = os.path.abspath(output)
    location, _ = os.path.split(output)
    if not os.path.isdir(location):
        os.makedirs(location)

    backports_required_latest = read_yaml_file(os.path.join(ETC_DIR, BACKPORTS_REQUIRED_FILE))

    # Get the state of the backports_required_for_multiversion_tests.yml file for the old
    # binary we are running tests against. We do this by using the commit hash from the old
    # mongo shell executable.
    from buildscripts.resmokelib import multiversionconstants

    shell_version = {
        MultiversionOptions.LAST_LTS: multiversionconstants.LAST_LTS_MONGO_BINARY,
        MultiversionOptions.LAST_CONTINUOUS: multiversionconstants.LAST_CONTINUOUS_MONGO_BINARY,
    }[old_bin_version]

    old_version_commit_hash = get_backports_required_hash_for_shell_version(
        mongo_shell_path=shell_version
    )

    # Get the yaml contents from the old commit.
    logger.info(f"Downloading file from commit hash of old branch {old_version_commit_hash}")
    backports_required_old = get_old_yaml(old_version_commit_hash)

    def diff(list1, list2):
        return [elem for elem in (list1 or []) if elem not in (list2 or [])]

    def get_suite_exclusions(version_key):
        _suites_latest = backports_required_latest[version_key]["suites"] or {}
        # Check if the changed syntax for etc/backports_required_for_multiversion_tests.yml has been
        # backported.
        # This variable and all branches where it's not set can be deleted after backporting the change.
        change_backported = version_key in backports_required_old.keys()
        if change_backported:
            _always_exclude = diff(
                backports_required_latest[version_key]["all"],
                backports_required_old[version_key]["all"],
            )
            _suites_old: defaultdict = defaultdict(
                list, backports_required_old[version_key]["suites"] or {}
            )
        else:
            _always_exclude = diff(
                backports_required_latest[version_key]["all"], backports_required_old["all"]
            )
            _suites_old: defaultdict = defaultdict(list, backports_required_old["suites"] or {})

        return _suites_latest, _suites_old, _always_exclude

    suites_latest, suites_old, always_exclude = get_suite_exclusions(
        old_bin_version.replace("_", "-")
    )

    tags = _tags.TagsConfig()

    # Tag tests that are excluded from every suite.
    for elem in always_exclude:
        tags.add_tag("js_test", elem["test_file"], BACKPORT_REQUIRED_TAG)

    # Tag tests that are excluded on a suite-by-suite basis.
    for suite in suites_latest.keys():
        test_set = set()
        for elem in diff(suites_latest[suite], suites_old[suite]):
            test_set.add(elem["test_file"])
        for test in test_set:
            tags.add_tag("js_test", test, f"{suite}_{BACKPORT_REQUIRED_TAG}")

    logger.info(f"Writing exclude tags to {output}.")
    tags.write_file(
        filename=output, preamble="Tag file that specifies exclusions from multiversion suites."
    )
