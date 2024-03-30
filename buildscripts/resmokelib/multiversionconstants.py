"""FCV and Server binary version constants used for multiversion testing."""
import structlog

from buildscripts.resmokelib.multiversion.multiversion_service import (
    MongoReleases, MongoVersion, MultiversionService, MONGO_VERSION_YAML, in_git_root_dir,
    get_mongo_git_version)

LAST_LTS = "last_lts"
LAST_CONTINUOUS = "last_continuous"

LOGGER = structlog.getLogger(__name__)


def generate_mongo_version_file():
    """Generate the mongo version data file. Should only be called in the root of the mongo directory."""
    version = get_mongo_git_version()

    # Write the current MONGO_VERSION to a data file.
    with open(MONGO_VERSION_YAML, 'w') as mongo_version_fh:
        mongo_version_fh.write("mongo_version: " + version)


if in_git_root_dir():
    generate_mongo_version_file()
else:
    LOGGER.info("Skipping generating mongo version file since we're not in the root of a git repo")


def evg_project_str(version):
    """Return the evergreen project name for the given version."""
    return 'mongodb-mongo-v{}.{}'.format(version.major, version.minor)


multiversion_service = MultiversionService(
    mongo_version=MongoVersion.generate(),
    mongo_releases=MongoReleases.generate(),
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

# Generate tags for all FCVS in (lastLTS, latest].
# All multiversion tests should be run with these tags excluded.
REQUIRES_FCV_TAG = version_constants.get_fcv_tag_list()

# Generate evergreen project names for all FCVs less than latest.
EVERGREEN_PROJECTS = ['mongodb-mongo-master']
EVERGREEN_PROJECTS.extend([evg_project_str(fcv) for fcv in version_constants.fcvs_less_than_latest])

OLD_VERSIONS = [
    LAST_LTS
] if LAST_CONTINUOUS_FCV == LAST_LTS_FCV or LAST_CONTINUOUS_FCV in version_constants.get_eols(
) else [LAST_LTS, LAST_CONTINUOUS]


def log_constants(exec_log):
    """Log FCV constants."""
    exec_log.info("Last LTS FCV: {}".format(LAST_LTS_FCV))
    exec_log.info("Last Continuous FCV: {}".format(LAST_CONTINUOUS_FCV))
    exec_log.info("Latest FCV: {}".format(LATEST_FCV))
    exec_log.info("Requires FCV Tag Latest: {}".format(REQUIRES_FCV_TAG_LATEST))
    exec_log.info("Requires FCV Tag: {}".format(REQUIRES_FCV_TAG))
