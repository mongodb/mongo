# Fetches the CI-built OpenSSL 3 archive (produced by the build_openssl3 task; see
# build_openssl.sh) for a custom build and points the compile at it, so the build
# host does not need a custom image with OpenSSL 3 preinstalled.
#
# The archive's rpaths are $ORIGIN-relative, so it extracts into the task workdir
# (no root access needed) and still resolves its own libraries. After extraction
# this script writes custom_openssl3_root.yml overriding mongo_openssl_root (and
# custom_ld_library_path) to the extracted location; a subsequent expansions.update
# applies the override for the rest of the task, so the bazel toolchain (see
# MONGO_OPENSSL_ROOT in bazel/toolchains/cc/mongo_linux/mongo_toolchain.bzl) compiles
# and links against the extracted OpenSSL.
#
# Configured by expansions:
#   custom_openssl3_remote_file  S3 key of the archive (required to enable)
#   custom_openssl3_sha256       sha256 of the archive (required when the above is
#                                set; build_openssl.sh prints it for the tarball it
#                                produces)
#   custom_openssl3_bucket       S3 bucket (default: mdb-build-public)
#
# The download is anonymous HTTPS: mdb-build-public is a publicly-readable bucket
# (the same way bazelisk/buildifier/ruff are fetched in buildscripts/), so no AWS
# credentials or boto3 are needed. The build_openssl3 task publishes new archive
# versions to the bucket automatically (with skip_existing, so a published
# openssl_version is never overwritten); if that is unavailable, seeding the
# archive manually with the devprod-build SSO profile is the fallback (see the
# custom_builds README).
#
# This script also runs on variants that do not configure the archive (it is part
# of the shared "bazel compile" function), in which case it writes a harmless
# override file so the unconditional expansions.update that follows has a file to
# read.
#
# The download retries for a while: the archive for a new OpenSSL version may be
# in the process of being uploaded when the compile starts.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set -o errexit
set -o verbose

if [ -z "${custom_openssl3_remote_file:-}" ]; then
    echo "custom_openssl3_remote_file is not set; writing empty override file"
    cat <<EOT >src/custom_openssl3_root.yml
custom_openssl3_fetch_configured: "false"
EOT
    exit 0
fi

: "${custom_openssl3_sha256:?custom_openssl3_sha256 expansion must be set when custom_openssl3_remote_file is set (build_openssl.sh prints the digest of the tarball it produces)}"

bucket="${custom_openssl3_bucket:-mdb-build-public}"
url="https://${bucket}.s3.amazonaws.com/${custom_openssl3_remote_file}"
echo "Fetching ${url}"

for attempt in $(seq 1 30); do
    if curl -fL --retry 3 -o openssl3.tgz "${url}"; then
        break
    fi
    echo "Download attempt ${attempt} failed; retrying in 60s (the archive may still be uploading)"
    sleep 60
done
if [ ! -f openssl3.tgz ]; then
    echo "Failed to download ${custom_openssl3_remote_file} after 30 attempts"
    exit 1
fi

# The download is anonymous, so pin the digest in the variant rather than trusting
# the bucket. skip_existing on the publish side keeps a published openssl_version
# immutable, so the pinned digest stays valid.
echo "${custom_openssl3_sha256}  openssl3.tgz" | sha256sum -c

tar -xzf openssl3.tgz
openssl_root="${workdir}/opt/openssl3"

# Normalize the libdir: the archive contains lib or lib64 depending on the platform
# it was built on, while the toolchain and runtime hooks reference both.
if [ -d "${openssl_root}/lib" ] && [ ! -e "${openssl_root}/lib64" ]; then
    ln -s lib "${openssl_root}/lib64"
fi
if [ -d "${openssl_root}/lib64" ] && [ ! -e "${openssl_root}/lib" ]; then
    ln -s lib64 "${openssl_root}/lib"
fi

# Relocatability smoke test: with no LD_LIBRARY_PATH, the $ORIGIN-relative rpaths
# must resolve libssl/libcrypto from the extracted location.
"${openssl_root}/bin/openssl" version

cat <<EOT >src/custom_openssl3_root.yml
mongo_openssl_root: "${openssl_root}"
custom_ld_library_path: "${openssl_root}/lib64:${openssl_root}/lib"
EOT
