# Smoke-tests the dist tarball produced by a custom build:
# - the tarball exists and contains a mongod binary,
# - mongod --version runs,
# - the reported version string carries the custom_build_version_suffix,
# - the reported allocator matches custom_build_verify_allocator (if set),
# - the ssl library linkage is printed for inspection.
#
# Usage: verify_dist.sh <path-to-dist-tarball>
#
# This branch has no evergreen/prelude.sh; expansions are passed in the
# environment (add_expansions_to_env: true).

set -o errexit
set -o verbose

tarball="${1:?usage: verify_dist.sh <path-to-dist-tarball>}"

if [ ! -f "${tarball}" ]; then
  echo "Expected dist tarball ${tarball} does not exist" >&2
  exit 1
fi

rm -rf custom_build_dist_verify
mkdir -p custom_build_dist_verify
tar -xf "${tarball}" -C custom_build_dist_verify

mongod_path=$(find custom_build_dist_verify -type f -name mongod | head -1)
if [ -z "${mongod_path}" ]; then
  echo "No mongod binary found in ${tarball}" >&2
  exit 1
fi

"${mongod_path}" --version

if [ -n "${custom_build_version_suffix:-}" ]; then
  if ! "${mongod_path}" --version | grep -q -- "${custom_build_version_suffix}"; then
    echo "mongod --version output is missing the custom build suffix '${custom_build_version_suffix}'" >&2
    exit 1
  fi
fi

# Assert the consumer-required allocator (for example custom_build_verify_allocator:
# jemalloc for a jemalloc build), so a build that silently fell back to the default
# allocator fails here instead of being delivered. A no-op unless the expansion is set.
if [ -n "${custom_build_verify_allocator:-}" ]; then
  if ! "${mongod_path}" --version | grep -qE -- "\"allocator\" *: *\"${custom_build_verify_allocator}\""; then
    echo "mongod --version output does not report allocator '${custom_build_verify_allocator}'" >&2
    exit 1
  fi
fi

ldd "${mongod_path}" | grep -i ssl || true

rm -rf custom_build_dist_verify
