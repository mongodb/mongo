# Stages the custom build's dist tarball for delivery and computes its remote
# destination (written to custom_build_upload.yml, applied by a subsequent
# expansions.update command).
#
# Commit builds are delivered to the consumer-facing location
# (${custom_build_upload_path_prefix}/${src_suffix}/mongodb-${push_arch}.${ext}).
# Patch builds are delivered to a patch-specific path instead, so that verification
# patches can exercise the upload end-to-end without overwriting the artifact the
# consumer picks up.
#
# Usage: prepare_upload.sh <path-to-dist-tarball>

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set -o errexit
set -o verbose

ext="${ext:-tgz}"
src_tarball="${1:?usage: prepare_upload.sh <path-to-dist-tarball>}"

if [ ! -f "${src_tarball}" ]; then
    echo "Expected dist tarball ${src_tarball} does not exist" >&2
    exit 1
fi

if [ "${is_patch:-}" = "true" ]; then
    remote_file="custom-build-patches/${build_variant}/${version_id}/mongodb-${push_arch}.${ext}"
else
    remote_file="${custom_build_upload_path_prefix}/${src_suffix}/mongodb-${push_arch}.${ext}"
fi

echo "Custom build dist tarball will be uploaded to ${custom_build_upload_bucket}/${remote_file}"

cp "${src_tarball}" "dist-stripped-custom.${ext}"

cat <<EOT >custom_build_upload.yml
custom_build_upload_remote_file: "${remote_file}"
EOT

cat custom_build_upload.yml
