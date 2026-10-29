# Smoke-tests the dist tarball produced by a custom build:
# - the tarball exists and contains a mongod binary,
# - mongod --version runs (binaries that need consumer-specific library locations find
#   them via the custom_ld_library_path hook in prelude.sh),
# - the reported version string carries the custom_build_version_suffix,
# - the ssl library linkage is printed for inspection.
#
# Usage: verify_dist.sh <path-to-dist-tarball>

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

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

# Assert the consumer-required library linkage (for example
# custom_build_verify_ssl_lib: libssl.so.3 for an OpenSSL 3 build), so a build
# that silently linked against the platform's default OpenSSL fails here instead
# of being delivered. A no-op unless the expansion is set.
if [ -n "${custom_build_verify_ssl_lib:-}" ]; then
    if ! ldd "${mongod_path}" | grep -q -- "${custom_build_verify_ssl_lib}"; then
        echo "mongod is not linked against '${custom_build_verify_ssl_lib}':" >&2
        ldd "${mongod_path}" >&2
        exit 1
    fi
fi

ldd "${mongod_path}" | grep -i ssl || true

rm -rf custom_build_dist_verify
