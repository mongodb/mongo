DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose

# Extract version from .bazelrc.target_mongo_version (e.g., "common --define=MONGO_VERSION=8.3.0-rc1003")
MONGO_VERSION="r$(grep -oP '(?<=MONGO_VERSION=)[^\s]+' .bazelrc.target_mongo_version)"

# If the project is sys-perf (or related), add the string -sys-perf to the version
if [[ "${project}" == sys-perf* ]]; then
    MONGO_VERSION="$MONGO_VERSION-sys-perf"
fi

# If this is a patch build, we add the patch version id to the version string so we know
# this build was a patch, and which evergreen task it came from
if [ "${is_patch}" = "true" ]; then
    MONGO_VERSION="$MONGO_VERSION-patch-${version_id}"
fi

# Forcefully override the version for purposes of testing against a different version than the
# branch is targeting.
#
# This disables all remote caching, since we're bypassing the check above that would mark the
# build as a development build.
#
# Artifacts from runs with this enabled still should not be used for a final (non-rc) public release
# unless the associated `test_packages` task has completed successfully.
if [[ -n "${MONGO_VERSION_OVERRIDE}" ]]; then
    MONGO_VERSION="${MONGO_VERSION_OVERRIDE}"
fi

# For commit builds, append the last 8 characters of the git revision to the version string.
if [[ "${requester}" == "commit" ]]; then
    GIT_REV=$(git rev-parse HEAD)
    MONGO_VERSION="${MONGO_VERSION}-${GIT_REV: -8}"
fi

echo "MONGO_VERSION = ${MONGO_VERSION}"

activate_venv
MONGO_VERSION=${MONGO_VERSION} IS_PATCH=${is_patch} IS_COMMIT_QUEUE=${is_commit_queue} $python buildscripts/generate_version_expansions.py --out version_expansions.yml
