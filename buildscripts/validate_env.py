#!/usr/bin/env python3
import contextlib
import os
import sys
import re
import urllib.request
from pathlib import Path
import git
import yaml

REQUIREMENTS_PATH = "buildscripts/requirements.txt"
GIT_ORG = "10gen"
ENTERPRISE_PATH = "src/mongo/db/modules/enterprise"
VENV_PATH = "python3-venv/bin/activate"
# alert user if less than 10gb of space left
STORAGE_AMOUNT = 10
# REQUIREMENTS_PATH = "buildscripts/requirements.txt"
LATEST_RELEASES = "https://raw.githubusercontent.com/mongodb/mongo/master/src/mongo/util/version/releases.yml"

# determine the path of the mongo directory
# this assumes the location of this script is in the buildscripts directory
buildscripts_path = os.path.dirname(os.path.realpath(__file__))
mongo_path = os.path.split(buildscripts_path)[0]
sys.path.append(mongo_path)

# pylint: disable=wrong-import-position
from site_scons.mongo.pip_requirements import verify_requirements, MissingRequirements
from buildscripts.resmokelib.utils import evergreen_conn


def check_cwd() -> int:
    print("Checking if current directory is mongo root directory...")
    if os.getcwd() != mongo_path:
        print("ERROR: We do not support building outside of the mongo root directory.")
        return 1

    return 0


# checks if the script is being run inside of a python venv or not
def in_virtualenv() -> bool:
    base_prefix = getattr(sys, "base_prefix", None) or getattr(sys, "real_prefix",
                                                               None) or sys.prefix
    return base_prefix != sys.prefix


def get_git_repo(path: str, repo_name: str) -> git.Repo:
    try:
        return git.Repo(path)
    except git.exc.NoSuchPathError:
        print(f"ERROR: Count not find {repo_name} git repo at {mongo_path}")
        print("Make sure your validate_env.py file is in the buildscripts directory")
        return None


# returns the hash of the most recent commit that is shared between upstream and HEAD
def get_common_hash(repo: git.Repo, repo_name: str) -> str:
    if not repo:
        return None

    upstream_remote = None

    # determine which remote is pointed to the 10gen github repo
    for remote in repo.remotes:
        if remote.url.endswith(f"{GIT_ORG}/{repo_name}.git"):
            upstream_remote = remote
            break

    if upstream_remote is None:
        print("ERROR: Could not find remote for:", f"{GIT_ORG}/{repo_name}")
        return None

    upstream_remote.fetch("master")
    common_hash = repo.merge_base("HEAD", f"{upstream_remote.name}/master")

    if not common_hash or len(common_hash) == 0:
        print(f"ERROR: Could not find common hash for {repo_name}")
        return None

    return common_hash[0]


def check_git_repos() -> int:
    print("Checking if mongo repo and enterprise module repo are in sync...")
    mongo_repo = get_git_repo(mongo_path, "mongo")
    mongo_hash = get_common_hash(mongo_repo, "mongo")
    enterprise_dir = os.path.join(mongo_path, ENTERPRISE_PATH)
    enterprise_repo = get_git_repo(enterprise_dir, "mongo-enterprise-modules")
    enterprise_hash = get_common_hash(enterprise_repo, "mongo-enterprise-modules")

    if not mongo_hash or not enterprise_hash:
        return 1

    evg_api = evergreen_conn.get_evergreen_api(Path.home() / '.evergreen.yml')
    manifest = evg_api.manifest("mongodb-mongo-master", mongo_hash)
    modules = manifest.modules
    if "enterprise" in modules and str(enterprise_hash) != modules["enterprise"].revision:
        synced_enterprise_hash = modules["enterprise"].revision
        print("Error: the mongo repo and enterprise module repo are out of sync")
        print(
            f"Try `git fetch; git rebase --onto {synced_enterprise_hash}` in the enterprise repo directory"
        )
        print(f"Your enterprise repo directory is {enterprise_dir}")
        return 1

    # Check if the git tag is out of date
    # https://mongodb.stackenterprise.co/questions/145
    print("Checking if your mongo repo git tag is up to date...")
    releases_page = urllib.request.urlopen(LATEST_RELEASES)
    page_bytes = releases_page.read()
    text = page_bytes.decode("utf-8")
    parsed_text = yaml.safe_load(text)
    compat_versions = parsed_text['featureCompatibilityVersions']
    if not compat_versions or len(compat_versions) <= 1:
        print(
            "ERROR: Something went wrong, there are not at least two feature compatibility mongo versions"
        )
        return 1
    else:
        # Hard coded to the second-to-last version because the last version should be the test version
        target_version = compat_versions[-2]
        local_version = mongo_repo.git.describe()
        # get the version info we want out of git describe
        trimmed_local_version = re.search("([0-9]+\\.[0-9]+)(\\.[0-9])?", local_version).group(1)
        if trimmed_local_version != target_version:
            print(
                "ERROR: Your git tag is out of date, run `git config remote.origin.tagOpt '--tags'; git fetch origin master`"
            )
            return 1

    return 0


# check for missing dependencies
def check_dependencies() -> int:
    print("Checking for missing dependencies...")
    requirements_file_path = os.path.join(mongo_path, REQUIREMENTS_PATH)
    try:
        with contextlib.redirect_stdout(None):
            verify_requirements(requirements_file_path)
    except MissingRequirements as ex:
        print(ex)
        print(
            f"ERROR: Found missing dependencies, run `python -m pip install -r {REQUIREMENTS_PATH}`"
        )
        if not in_virtualenv():
            print(
                "WARNING: you are not in a python venv, we recommend using one to handle your requirements."
            )
        return 1

    return 0


def check_space() -> int:
    print("Checking if there is enough disk space to build...")
    # Get the filesystem information where the mongo directory lies
    statvfs = os.statvfs(mongo_path)
    free_bytes = statvfs.f_frsize * statvfs.f_bfree
    free_gb = (free_bytes // 1000) / 1000

    # Warn if there is a low amount of space left in the filesystem
    if free_gb < STORAGE_AMOUNT:
        print(f"WARNING: only {free_gb}GB of space left in filesystem")


def main() -> int:
    if any([check_cwd(), check_git_repos(), check_dependencies(), check_space()]):
        exit(1)


if __name__ == '__main__':
    main()

# More requirements can be added as new issues appear
