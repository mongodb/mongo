cd src

set -o errexit
set -o verbose

# Extract version from .bazelrc.target_mongo_version (e.g., "common --define=MONGO_VERSION=8.2.2")
version="r$(grep -oP '(?<=MONGO_VERSION=)[^\s]+' .bazelrc.target_mongo_version)"

# For commit builds, append the last 8 characters of the git revision to the version string.
if [[ "${requester:-}" == "commit" ]]; then
    GIT_REV=$(git rev-parse HEAD)
    version="${version}-${GIT_REV: -8}"
fi

if [ ${IS_RELEASE} = 'true' ]; then
    version="${version#r}"
fi

cat <<EOT >papertrail-expansions.yml
release_version: "$version"
EOT
cat papertrail-expansions.yml
