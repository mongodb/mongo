"""FCV and Server binary version constants used for multiversion testing."""
import os
import shutil
from subprocess import call, CalledProcessError, check_output, STDOUT, DEVNULL
import structlog

try:
    # when running resmoke
    from buildscripts.resmokelib.multiversion.multiversion_service import MongoReleases, MongoVersion, MultiversionService
    from buildscripts.resmokelib.multiversionsetupconstants import USE_EXISTING_RELEASES_FILE
except ImportError:
    # when running db-contrib-tool
    from multiversionsetupconstants import USE_EXISTING_RELEASES_FILE
    from multiversion.multiversion_service import MongoReleases, MongoVersion, MultiversionService

LOGGER = structlog.getLogger(__name__)

# These values must match the include paths for artifacts.tgz in evergreen.yml.
MONGO_VERSION_YAML = ".resmoke_mongo_version.yml"
RELEASES_YAML = ".resmoke_mongo_release_values.yml"


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


def generate_releases_file():
    """Generate the releases constants file."""
    # Copy the 'releases.yml' file from the source tree.
    releases_yaml_path = os.path.join("src", "mongo", "util", "version", "releases.yml")
    if not os.path.isfile(releases_yaml_path):
        LOGGER.info(
            'Skipping yml file generation because file .resmoke_mongo_release_values.yml does not exist at path {}.'
            .format(releases_yaml_path))
        return

    shutil.copyfile(releases_yaml_path, RELEASES_YAML)


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


def version_str(version):
    """Return a string of the given version in 'MAJOR.MINOR' form."""
    return '{}.{}'.format(version.major, version.minor)


def evg_project_str(version):
    """Return the evergreen project name for the given version."""
    return 'mongodb-mongo-v{}.{}'.format(version.major, version.minor)


multiversion_service = MultiversionService(
    mongo_version=MongoVersion.from_yaml_file(MONGO_VERSION_YAML),
    mongo_releases=MongoReleases.from_yaml_file(RELEASES_YAML),
)

fcv_constants = multiversion_service.calculate_fcv_constants()

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

REQUIRES_FCV_TAG_LATEST = fcv_constants.get_latest_tag()

# Generate tags for all FCVS in (lastLTS, latest], or (lowerBoundOverride, latest] if requested.
# All multiversion tests should be run with these tags excluded.
REQUIRES_FCV_TAG = fcv_constants.get_fcv_tag_list()

# Generate evergreen project names for all FCVs less than latest.
EVERGREEN_PROJECTS = ['mongodb-mongo-master']
EVERGREEN_PROJECTS.extend([evg_project_str(fcv) for fcv in fcv_constants.fcvs_less_than_latest])

OLD_VERSIONS = ["last_lts"]
if LAST_LTS_FCV != LAST_CONTINUOUS_FCV:
    OLD_VERSIONS.append("last_continuous")
