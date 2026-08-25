# Computes MONGO_VERSION_OVERRIDE for a custom build and writes it to
# custom_build_version.yml (applied by a subsequent expansions.update) so the
# override takes effect for this task.
#
# The override is the base version plus the consumer-specific suffix in the
# custom_build_version_suffix expansion, e.g. "r4.4.31-mcas-jemalloc". The base
# version is taken from the custom_build_base_version expansion, which every
# custom build variant on this branch must set (the version committed in
# .bazelrc.target_mongo_version lags the latest release).
#
# This hook runs as part of the shared "generate compile expansions" function,
# right before the inline shell that generates the compile expansions, which
# already honors MONGO_VERSION_OVERRIDE and folds it into the generated version
# expansion. Every compile task applies those compile expansions, so every
# binary carries the custom version.
#
# This script also runs on variants that do not configure a custom build (it is
# part of the shared "generate compile expansions" function), in which case it
# writes a harmless override file so the unconditional expansions.update that
# follows has a file to read.
#
# This branch has no evergreen/prelude.sh; expansions are passed in the
# environment (add_expansions_to_env: true).

set -o errexit
set -o verbose

# Run from the repository root regardless of the invoking working directory.
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." > /dev/null 2>&1 && pwd)"

if [ -z "${custom_build_version_suffix:-}" ]; then
  echo "custom_build_version_suffix is not set; writing empty override file"
  cat << EOT > custom_build_version.yml
custom_build_version_override_configured: "false"
EOT
  exit 0
fi

if [ -z "${custom_build_base_version:-}" ]; then
  echo "custom_build_version_suffix is set but custom_build_base_version is not" >&2
  exit 1
fi

MONGO_VERSION_OVERRIDE="r${custom_build_base_version#r}-${custom_build_version_suffix}"
echo "MONGO_VERSION_OVERRIDE = ${MONGO_VERSION_OVERRIDE}"

cat << EOT > custom_build_version.yml
MONGO_VERSION_OVERRIDE: "${MONGO_VERSION_OVERRIDE}"
EOT

cat custom_build_version.yml
