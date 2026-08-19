# Bundles the CI-built OpenSSL 3 runtime libraries (libssl.so.3, libcrypto.so.3)
# next to the produced binaries, so they resolve their OpenSSL 3 dependency via
# their built-in $ORIGIN/../lib RUNPATH (see mongo_src_rules.bzl) instead of
# needing LD_LIBRARY_PATH set by the environment.
#
# Two layouts are handled:
#   - the bazel install tree (bazel-bin/install/{bin,lib}): binaries run directly
#     from the compile task's workdir (e.g. run_dbtest) find the libraries here.
#   - the dist-test tarball passed as an argument (e.g. dist-test-stripped.tgz):
#     the libraries are added under the archive's top-level lib/ directory, so
#     every test task that downloads and extracts mongo-binaries.tgz gets a
#     self-contained tree.
#
# No-op unless custom_openssl3_remote_file is set (custom builds). The plain dist
# tarball (the custom build's deliverable) is intentionally NOT bundled: the
# consumer's hosts provide OpenSSL 3 themselves, and custom_build_deliver still
# fetches the archive for its smoke test.
#
# Only the SONAME'd runtime libraries are bundled. Provider modules under
# ossl-modules/ (e.g. legacy.so) are not: mongod uses the default provider built
# into libcrypto, and the compiled-in MODULESDIR would not resolve on test hosts
# anyway (same behavior as the LD_LIBRARY_PATH approach this replaces).

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set -o errexit
set -o verbose

if [ -z "${custom_openssl3_remote_file:-}" ]; then
    echo "custom_openssl3_remote_file is not set; nothing to bundle"
    exit 0
fi

: "${mongo_openssl_root:?mongo_openssl_root must be set (run fetch_openssl3.sh first)}"

libs_dir="${mongo_openssl_root}/lib64"
if [ ! -d "${libs_dir}" ]; then
    libs_dir="${mongo_openssl_root}/lib"
fi

bundle_libs() {
    dest="$1"
    mkdir -p "${dest}"
    cp -L "${libs_dir}/libssl.so.3" "${libs_dir}/libcrypto.so.3" "${dest}/"
}

# The install tree, when this task produced one.
if [ -d src/bazel-bin/install/bin ]; then
    echo "Bundling OpenSSL 3 runtime into bazel-bin/install/lib"
    bundle_libs src/bazel-bin/install/lib
fi

# Any tarballs passed as arguments (e.g. bazel-bin/dist-test-stripped.tgz).
for tarball in "$@"; do
    echo "Bundling OpenSSL 3 runtime into ${tarball}"
    tmp="$(mktemp -d)"
    tar -xzf "${tarball}" -C "${tmp}"
    top="$(ls "${tmp}")"
    if [ ! -d "${tmp}/${top}/bin" ]; then
        echo "${tarball} does not have the expected <top>/bin layout (found: ${top})"
        exit 1
    fi
    bundle_libs "${tmp}/${top}/lib"
    tar -czf "${tarball}.new" -C "${tmp}" "${top}"
    mv "${tarball}.new" "${tarball}"
    rm -rf "${tmp}"
done

# Smoke test: a bundled binary must resolve OpenSSL 3 with no LD_LIBRARY_PATH.
if [ -d src/bazel-bin/install/lib ] && [ -x src/bazel-bin/install/bin/mongod ]; then
    env -u LD_LIBRARY_PATH src/bazel-bin/install/bin/mongod --version
fi
