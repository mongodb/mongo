cd src

set -o errexit
set -o verbose

# Extract version from .bazelrc.target_mongo_version (e.g., "common --define=MONGO_VERSION=8.2.2")
target_mongo_version=$(awk -F'MONGO_VERSION=' '/MONGO_VERSION=/ { split($2, version, /[[:space:]]/); print version[1]; exit }' .bazelrc.target_mongo_version)
if [[ -z "${target_mongo_version}" ]]; then
    echo "Unable to extract MONGO_VERSION from .bazelrc.target_mongo_version" >&2
    exit 1
fi
version="r${target_mongo_version}"

# For commit builds, append the first 8 characters of the git revision to the version string.
if [[ "${requester:-}" == "commit" ]]; then
    GIT_REV=$(git rev-parse HEAD)
    version="${version}-${GIT_REV:0:8}"
fi

if [ ${IS_RELEASE} = 'true' ]; then
    version="${version#r}"
fi

cat <<EOT >papertrail-expansions.yml
release_version: "$version"
EOT
cat papertrail-expansions.yml
