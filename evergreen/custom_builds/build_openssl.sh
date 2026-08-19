# Builds OpenSSL from source and packs the installed tree into a tarball, mirroring
# how the buildhost images provision /opt/openssl3 (see the sslv3_compile role in
# 10gen/buildhost-configuration). The resulting tarball extracts to
# opt/openssl3/... and can be unpacked at /opt/openssl3 to reproduce the image
# contents on any compatible host.
#
# Unlike the buildhost role, the rpath is baked in as $ORIGIN-relative paths rather
# than the install prefix, so the extracted tree is relocatable: consumers can unpack
# it anywhere (for example a task workdir, when the task has no root access to
# /opt/openssl3) and the libraries and binaries still resolve each other without
# LD_LIBRARY_PATH. Both lib and lib64 are listed because the install libdir varies by
# platform (rhel8 x86_64 installs into lib64). The \$$ escaping is load-bearing:
# shell single-quotes pass \$$ORIGIN to Configure, which writes it verbatim into
# the Makefile's LDFLAGS; make expands $$ to $ when expanding recipes (giving
# \$ORIGIN), and the recipe shell unescapes \$ to a literal $ORIGIN for the
# linker. Both failure modes were observed in CI: plain '$ORIGIN' reaches the
# recipe shell unescaped and is expanded as an empty environment variable (baked
# rpath /../lib), while '\$ORIGIN' loses the $O entirely because make treats $O
# as a make variable reference (baked rpath \RIGIN/../lib).
#
# Configured by expansions:
#   openssl_version       e.g. openssl-3.4.0
#   openssl_source_url    URL of the source tarball
#   openssl_source_sha256 sha256 of the source tarball
#
# The install prefix is fixed to /opt/openssl3 (baked into the build via --prefix)
# while the files are staged with DESTDIR so no root access is needed.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set -o errexit
set -o verbose

: "${openssl_version:?openssl_version expansion must be set}"
: "${openssl_source_url:?openssl_source_url expansion must be set}"
: "${openssl_source_sha256:?openssl_source_sha256 expansion must be set}"

prefix=/opt/openssl3
workdir="$(pwd)"

if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
    # Fall back to the mongo toolchain's compiler if the image has no system one.
    export PATH="$(echo /opt/mongodbtoolchain/v*/bin | tr ' ' ':')${PATH:+:${PATH}}"
fi

curl -fL --retry 3 -o "${openssl_version}.tar.gz" "${openssl_source_url}"
echo "${openssl_source_sha256}  ${openssl_version}.tar.gz" | sha256sum -c

rm -rf openssl-build openssl-pkg
mkdir -p openssl-build openssl-pkg
tar -xf "${openssl_version}.tar.gz" -C openssl-build

cd "openssl-build/${openssl_version}"
./Configure \
    --prefix="${prefix}" \
    --openssldir="${prefix}" \
    -Wl,--enable-new-dtags,-z,origin,-rpath,'\$$ORIGIN/../lib:\$$ORIGIN/../lib64'
make -j"$(nproc)"
make install DESTDIR="${workdir}/openssl-pkg"

cd "${workdir}"
# Smoke-test the staged build with no LD_LIBRARY_PATH: the $ORIGIN-relative rpath
# must resolve the libraries from the staging dir on its own, proving the tarball is
# relocatable.
"openssl-pkg${prefix}/bin/openssl" version

tar -czf openssl3.tgz -C openssl-pkg "opt/openssl3"
ls -la openssl3.tgz
# Print the digest so it can be pinned as the custom_openssl3_sha256 expansion of
# the consuming custom build variants (fetch_openssl3.sh verifies the downloaded
# archive against it).
sha256sum openssl3.tgz
