# Bundles the CI-built OpenSSL 3 runtime libraries (libssl.so.3, libcrypto.so.3)
# next to the produced binaries, so they resolve their OpenSSL 3 dependency via
# their built-in $ORIGIN/../lib RUNPATH (added by SConstruct when
# MONGO_OPENSSL_ROOT is set) instead of needing LD_LIBRARY_PATH set by the
# environment.
#
# Two layouts are handled:
#   - the scons install trees (src/build/install/{bin,lib} and, with
#     PREFIX=dist-test, src/build/install/dist-test/{bin,lib}): binaries run
#     directly from the compile task's workdir (e.g. run_dbtest, which runs
#     against build/install/bin) find the libraries here.
#   - the dist-test tarball passed as an argument (e.g. mongodb-binaries.tgz):
#     the libraries are added under the archive's <top>/lib directory (top is
#     dist-test), so every test task that downloads and extracts
#     mongo-binaries.tgz gets a self-contained tree.
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

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
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

# The install trees, when this task produced them. compile tasks install under
# src/build/install/bin (src/build/install/dist-test/bin with PREFIX=dist-test).
for install_tree in src/build/install src/build/install/dist-test; do
  if [ -d "${install_tree}/bin" ]; then
    echo "Bundling OpenSSL 3 runtime into ${install_tree}/lib"
    bundle_libs "${install_tree}/lib"
  fi
done

# Any tarballs passed as arguments (e.g. src/mongodb-binaries.tgz).
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
for mongod in src/build/install/dist-test/bin/mongod src/build/install/bin/mongod; do
  if [ -x "${mongod}" ] && [ -d "$(dirname "${mongod}")/../lib" ]; then
    env -u LD_LIBRARY_PATH "${mongod}" --version
    break
  fi
done
