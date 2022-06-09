DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose
# We get the raw version string (r1.2.3-45-gabcdef) from git
MONGO_VERSION=$(git describe --abbrev=7)
# If this is a patch build, we add the patch version id to the version string so we know
# this build was a patch, and which evergreen task it came from
if [ "${is_patch}" = "true" ]; then
  MONGO_VERSION="$MONGO_VERSION-patch-${version_id}"
fi
echo "MONGO_VERSION = ${MONGO_VERSION}"

activate_venv
MONGO_VERSION=${MONGO_VERSION} IS_PATCH=${is_patch} IS_COMMIT_QUEUE=${is_commit_queue} $python buildscripts/generate_version_expansions.py --out version_expansions.yml
