"""FCV and Server binary version constants used for multiversion testing."""
import os
import shutil
from subprocess import DEVNULL, STDOUT, CalledProcessError, call, check_output
import http
import requests
from retry import retry

import structlog

from buildscripts.resmokelib.multiversion.multiversion_service import (
    MongoReleases, MongoVersion, MultiversionService, MONGO_VERSION_YAML, RELEASES_YAML)
from buildscripts.resmokelib.multiversionsetupconstants import \
    USE_EXISTING_RELEASES_FILE

LAST_LTS = "last_lts"
LAST_CONTINUOUS = "last_continuous"

# We use the "releases.yml" file from "master" because it is guaranteed to be up-to-date
# with the latest EOL versions. If a "last-continuous" version is EOL, we don't include
# it in the multiversion config and therefore don't test against it.
MASTER_RELEASES_REMOTE_FILE = "https://raw.githubusercontent.com/mongodb/mongo/master/src/mongo/util/version/releases.yml"

LOGGER = structlog.getLogger(__name__)


def generate_mongo_version_file():
    """Generate the mongo version data file. Should only be called in the root of the mongo directory."""
    try:
        res = check_output("git describe", shell=True, text=True)
    except CalledProcessError as exp:
        raise ChildProcessError("Failed to run git describe to get the latest tag") from exp

    # Write the current MONGO_VERSION to a data file.
    with open(MONGO_VERSION_YAML, 'w') as mongo_version_fh:
        # E.g. res = 'r5.1.0-alpha-597-g8c345c6693\n'
        res = res[1:]  # Remove the leading "r" character.
        mongo_version_fh.write("mongo_version: " + res)


@retry(tries=5, delay=3)
def generate_releases_file():
    """Generate the releases constants file."""
    # Copy the 'releases.yml' file from the source tree.
    with open(RELEASES_YAML, "wb") as file:
        response = requests.get(MASTER_RELEASES_REMOTE_FILE)
        if response.status_code != http.HTTPStatus.OK:
            raise RuntimeError(
                f"Fetching releases.yml file returned unsuccessful status: {response.status_code}, "
                f"response body: {response.text}\n")
        file.write(response.content)


def in_git_root_dir():
    """Return True if we are in the root of a git directory."""
    if call(["git", "branch"], stderr=STDOUT, stdout=DEVNULL) != 0:
        # We are not in a git directory.
        return False

    git_root_dir = check_output("git rev-parse --show-toplevel", shell=True, text=True).strip()
    # Always use forward slash for the cwd path to resolve inconsistent formatting with Windows.
    curr_dir = os.getcwd().replace("\\", "/")
    return git_root_dir == curr_dir


if in_git_root_dir():
    generate_mongo_version_file()
else:
    LOGGER.info("Skipping generating mongo version file since we're not in the root of a git repo")

# Avoiding regenerating the releases file if this flag is set. Should only be set if there are
# multiple processes attempting to set up multiversion concurrently.
if not USE_EXISTING_RELEASES_FILE:
    generate_releases_file()
else:
    LOGGER.info(
        "Skipping generating releases file since the --useExistingReleasesFile flag has been set")


def evg_project_str(version):
    """Return the evergreen project name for the given version."""
    return 'mongodb-mongo-v{}.{}'.format(version.major, version.minor)


multiversion_service = MultiversionService(
    mongo_version=MongoVersion.from_yaml_file(MONGO_VERSION_YAML),
    mongo_releases=MongoReleases.from_yaml_file(RELEASES_YAML),
)

version_constants = multiversion_service.calculate_version_constants()

LAST_LTS_BIN_VERSION = version_constants.get_last_lts_fcv()
LAST_CONTINUOUS_BIN_VERSION = version_constants.get_last_continuous_fcv()

LAST_LTS_FCV = version_constants.get_last_lts_fcv()
LAST_CONTINUOUS_FCV = version_constants.get_last_continuous_fcv()
LATEST_FCV = version_constants.get_latest_fcv()

LAST_CONTINUOUS_MONGO_BINARY = version_constants.build_last_continuous_binary("mongo")
LAST_CONTINUOUS_MONGOD_BINARY = version_constants.build_last_continuous_binary("mongod")
LAST_CONTINUOUS_MONGOS_BINARY = version_constants.build_last_continuous_binary("mongos")

LAST_LTS_MONGO_BINARY = version_constants.build_last_lts_binary("mongo")
LAST_LTS_MONGOD_BINARY = version_constants.build_last_lts_binary("mongod")
LAST_LTS_MONGOS_BINARY = version_constants.build_last_lts_binary("mongos")

REQUIRES_FCV_TAG_LATEST = version_constants.get_latest_tag()

# Generate tags for all FCVS in (lastLTS, latest], or (lowerBoundOverride, latest] if requested.
# All multiversion tests should be run with these tags excluded.
REQUIRES_FCV_TAG = version_constants.get_fcv_tag_list()

# Generate evergreen project names for all FCVs less than latest.
EVERGREEN_PROJECTS = ['mongodb-mongo-master']
EVERGREEN_PROJECTS.extend([evg_project_str(fcv) for fcv in version_constants.fcvs_less_than_latest])

OLD_VERSIONS = [
    LAST_LTS
] if LAST_CONTINUOUS_FCV == LAST_LTS_FCV or LAST_CONTINUOUS_FCV in version_constants.get_eols(
) else [LAST_LTS, LAST_CONTINUOUS]
