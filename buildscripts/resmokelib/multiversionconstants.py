"""FCV and Server binary version constants used for multiversion testing."""

from bisect import bisect_left, bisect_right
import os
import re
import shutil
from subprocess import call, CalledProcessError, check_output, STDOUT
import structlog
import yaml

from packaging.version import Version

LOGGER = structlog.getLogger(__name__)

# These values must match the include paths for artifacts.tgz in evergreen.yml.
MONGO_VERSION_YAML = ".resmoke_mongo_version.yml"
RELEASES_YAML = ".resmoke_mongo_release_values.yml"


def generate_data_files():
    """Generate the yaml files. Should only be called if the git repo is installed."""
    # Copy the 'releases.yml' file from the source tree.
    releases_yaml_path = os.path.join("src", "mongo", "util", "version", "releases.yml")
    if not os.path.isfile(releases_yaml_path):
        LOGGER.warning(
            'Expected file .resmoke_mongo_release_values.yml does not exist at path {}'.format(
                releases_yaml_path))
    shutil.copyfile(releases_yaml_path, RELEASES_YAML)

    try:
        res = check_output("git describe", shell=True, text=True)
    except CalledProcessError as exp:
        raise ChildProcessError("Failed to run git describe to get the latest tag") from exp

    # Write the current MONGO_VERSION to a data file.
    with open(MONGO_VERSION_YAML, 'w') as mongo_version_fh:
        # E.g. res = 'r5.1.0-alpha-597-g8c345c6693\n'
        res = res[1:]  # Remove the leading "r" character.
        mongo_version_fh.write("mongo_version: " + res)


def in_git_root_dir():
    """Return True if we are in the root of a git directory."""
    if call(["git", "branch"], stderr=STDOUT, stdout=open(os.devnull, 'w')) != 0:
        # We are not in a git directory.
        return False

    git_root_dir = check_output("git rev-parse --show-toplevel", shell=True, text=True).strip()
    # Always use forward slash for the cwd path to resolve inconsistent formatting with Windows.
    curr_dir = os.getcwd().replace("\\", "/")
    return git_root_dir == curr_dir


if in_git_root_dir():
    generate_data_files()
else:
    LOGGER.info("Skipping generating version constants since we're not in the root of a git repo")


class FCVConstantValues(object):
    """Object to hold the calculated FCV constants."""

    def __init__(self, latest, last_continuous, last_lts, requires_fcv_tag_list,
                 fcvs_less_than_latest):
        """
        Initialize the object.

        :param latest: Latest FCV.
        :param last_continuous: Last continuous FCV.
        :param last_lts: Last LTS FCV.
        :param requires_fcv_tag_list: List of FCVs that we need to generate a tag for.
        :param fcvs_less_than_latest: List of all FCVs that are less than latest, starting from v4.0.
        """
        self.latest = latest
        self.last_continuous = last_continuous
        self.last_lts = last_lts
        self.requires_fcv_tag_list = requires_fcv_tag_list
        self.fcvs_less_than_latest = fcvs_less_than_latest


def calculate_fcv_constants():
    """Calculate multiversion constants from data files."""
    mongo_version_yml_file = open(MONGO_VERSION_YAML, 'r')
    mongo_version_yml = yaml.safe_load(mongo_version_yml_file)
    mongo_version = mongo_version_yml['mongo_version']
    latest = Version(re.sub(r'-.*', '', mongo_version))

    releases_yml_file = open(RELEASES_YAML, 'r')
    releases_yml = yaml.safe_load(releases_yml_file)

    mongo_version_yml_file.close()
    releases_yml_file.close()

    fcvs = releases_yml['featureCompatibilityVersions']
    fcvs = list(map(Version, fcvs))
    lts = releases_yml['majorReleases']
    lts = list(map(Version, lts))

    # Highest release less than latest.
    last_continuous = fcvs[bisect_left(fcvs, latest) - 1]

    # Highest LTS release less than latest.
    last_lts = lts[bisect_left(lts, latest) - 1]

    # All FCVs greater than last LTS, up to latest.
    requires_fcv_tag_list = fcvs[bisect_right(fcvs, last_lts):bisect_right(fcvs, latest)]

    # All FCVs less than latest.
    fcvs_less_than_latest = fcvs[:bisect_left(fcvs, latest)]

    return FCVConstantValues(latest, last_continuous, last_lts, requires_fcv_tag_list,
                             fcvs_less_than_latest)


def version_str(version):
    """Return a string of the given version in 'MAJOR.MINOR' form."""
    return '{}.{}'.format(version.major, version.minor)


def tag_str(version):
    """Return a tag for the given version."""
    return 'requires_fcv_{}{}'.format(version.major, version.minor)


def evg_project_str(version):
    """Return the evergreen project name for the given version."""
    return 'mongodb-mongo-v{}.{}'.format(version.major, version.minor)


fcv_constants = calculate_fcv_constants()

LAST_LTS_BIN_VERSION = version_str(fcv_constants.last_lts)
LAST_CONTINUOUS_BIN_VERSION = version_str(fcv_constants.last_continuous)

LAST_LTS_FCV = version_str(fcv_constants.last_lts)
LAST_CONTINUOUS_FCV = version_str(fcv_constants.last_continuous)
LATEST_FCV = version_str(fcv_constants.latest)

LAST_CONTINUOUS_MONGO_BINARY = "mongo-" + LAST_CONTINUOUS_BIN_VERSION
LAST_CONTINUOUS_MONGOD_BINARY = "mongod-" + LAST_CONTINUOUS_BIN_VERSION
LAST_CONTINUOUS_MONGOS_BINARY = "mongos-" + LAST_CONTINUOUS_BIN_VERSION

LAST_LTS_MONGO_BINARY = "mongo-" + LAST_LTS_BIN_VERSION
LAST_LTS_MONGOD_BINARY = "mongod-" + LAST_LTS_BIN_VERSION
LAST_LTS_MONGOS_BINARY = "mongos-" + LAST_LTS_BIN_VERSION

REQUIRES_FCV_TAG_LATEST = tag_str(fcv_constants.latest)

# Generate tags for all FCVS in (lastLTS, latest].
# All multiversion tests should be run with these tags excluded.
REQUIRES_FCV_TAG = [tag_str(fcv) for fcv in fcv_constants.requires_fcv_tag_list]

# Generate evergreen project names for all FCVs less than latest.
EVERGREEN_PROJECTS = ['mongodb-mongo-master']
EVERGREEN_PROJECTS.extend([evg_project_str(fcv) for fcv in fcv_constants.fcvs_less_than_latest])
