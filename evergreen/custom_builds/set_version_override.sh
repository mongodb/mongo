# Computes MONGO_VERSION_OVERRIDE for a custom build and regenerates the version
# expansions locally so the override takes effect for this task.
#
# The override is the base version plus the consumer-specific suffix in the
# custom_build_version_suffix expansion, e.g. "r8.0.28-splunk-openssl3-rhel8".
# The base version defaults to the branch's target version from
# .bazelrc.target_mongo_version; fixed-version custom builds can set
# custom_build_base_version to override it explicitly.
#
# The shared per-version expansions file downloaded by "get version expansions"
# is generated once per Evergreen version and does not know about the override,
# so when an override is configured this script re-runs
# version_expansions_generate.sh in-process (which reads MONGO_VERSION_OVERRIDE
# from its environment) to overwrite src/version_expansions.yml before the
# "apply version expansions" step applies it.
#
# The override is also written to custom_build_version.yml (applied by a
# subsequent expansions.update) so later steps see MONGO_VERSION_OVERRIDE as a
# task expansion: bazel_evergreen_shutils::maybe_release_flag keys off it to
# force --config=public-release.
#
# This script also runs on variants that do not configure a custom build (it is
# part of the shared "bazel compile" function), in which case it writes a
# harmless override file so the unconditional expansions.update that follows has
# a file to read.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose

if [ -z "${custom_build_version_suffix:-}" ]; then
    echo "custom_build_version_suffix is not set; writing empty override file"
    cat <<EOT >custom_build_version.yml
custom_build_version_override_configured: "false"
EOT
    exit 0
fi

if [ -n "${custom_build_base_version:-}" ]; then
    base_version="${custom_build_base_version}"
else
    # Extract version from .bazelrc.target_mongo_version (e.g., "common --define=MONGO_VERSION=8.0.28")
    base_version=$(awk -F'MONGO_VERSION=' '/MONGO_VERSION=/ { split($2, version, /[[:space:]]/); print version[1]; exit }' .bazelrc.target_mongo_version)
fi

if [ -z "${base_version}" ]; then
    echo "Unable to determine the base version for the custom build" >&2
    exit 1
fi

MONGO_VERSION_OVERRIDE="r${base_version#r}-${custom_build_version_suffix}"
echo "MONGO_VERSION_OVERRIDE = ${MONGO_VERSION_OVERRIDE}"

cat <<EOT >custom_build_version.yml
MONGO_VERSION_OVERRIDE: "${MONGO_VERSION_OVERRIDE}"
EOT

cat custom_build_version.yml

# Regenerate the version expansions locally, overwriting the shared per-version
# file. version_expansions_generate.sh loads prelude.sh itself (which requires
# the task workdir), so run it from there with the override exported.
cd "${workdir}"
export MONGO_VERSION_OVERRIDE
bash src/evergreen/functions/version_expansions_generate.sh
