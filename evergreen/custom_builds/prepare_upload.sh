# Stages the custom build's dist tarball for delivery: computes the destination,
# writes a manifest (version, git tag, revision, sha256, S3 URIs, Evergreen task)
# to upload next to it, and emits custom_build_upload.yml for the
# expansions.update that follows. Destination by delivery channel:
#
# - release (${triggered_by_git_tag} set): immutable
#   ${custom_build_upload_path_prefix}/${version}/mongodb-${push_arch}.${ext};
#   the version must match the tag exactly.
# - branch-tracking (commit/cron): rolling ${custom_build_upload_path_prefix}/
#   ${src_suffix}/..., overwritten by every build of the branch tip.
# - patch: custom-build-patches/..., never touching consumer-facing paths.
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

for required in custom_build_upload_bucket custom_build_upload_path_prefix push_arch version; do
    if [ -z "${!required:-}" ]; then
        echo "Expansion ${required} is not set; cannot compute the delivery destination" >&2
        exit 1
    fi
done

tarball_name="mongodb-${push_arch}.${ext}"
# skip_existing makes release deliveries immutable; the other channels overwrite.
skip_existing="false"
if [ "${is_patch:-}" = "true" ]; then
    delivery_channel="patch"
    remote_dir="custom-build-patches/${build_variant}/${version_id}"
elif [ -n "${triggered_by_git_tag:-}" ]; then
    delivery_channel="release"
    # Only the tagged release plus the consumer suffix may be published under
    # this release path — not a mis-tagged commit or a different suffix.
    tag_version="${triggered_by_git_tag#r}"
    expected_version="${tag_version}${custom_build_version_suffix:+-${custom_build_version_suffix}}"
    if [ "${version}" != "${expected_version}" ]; then
        echo "Git tag ${triggered_by_git_tag} expects custom build version ${expected_version}, got ${version}" >&2
        exit 1
    fi
    remote_dir="${custom_build_upload_path_prefix}/${version}"
    skip_existing="true"
else
    delivery_channel="branch-tracking"
    remote_dir="${custom_build_upload_path_prefix}/${src_suffix:?src_suffix is not set}"
fi
remote_file="${remote_dir}/${tarball_name}"
manifest_remote_file="${remote_dir}/${tarball_name%."${ext}"}.manifest.json"

echo "Custom build dist tarball will be uploaded to ${custom_build_upload_bucket}/${remote_file} (${delivery_channel})"

cp "${src_tarball}" "dist-stripped-custom.${ext}"

activate_venv
# python for correct JSON escaping; expansions passed explicitly because
# prelude.sh defines them as unexported shell variables.
DELIVERY_CHANNEL="${delivery_channel}" TARBALL_NAME="${tarball_name}" TARBALL_PATH="dist-stripped-custom.${ext}" \
    REMOTE_FILE="${remote_file}" MANIFEST_REMOTE_FILE="${manifest_remote_file}" \
    custom_build_upload_bucket="${custom_build_upload_bucket}" version="${version}" \
    custom_build_version_suffix="${custom_build_version_suffix:-}" triggered_by_git_tag="${triggered_by_git_tag:-}" \
    revision="${revision:-}" branch_name="${branch_name:-}" project="${project:-}" \
    build_variant="${build_variant:-}" push_arch="${push_arch}" task_id="${task_id:-}" \
    version_id="${version_id:-}" build_id="${build_id:-}" execution="${execution:-}" \
    "$python" - <<'EOF'
import datetime
import hashlib
import json
import os


def env(name, default=""):
    return os.environ.get(name, default)


tarball = env("TARBALL_PATH")
digest = hashlib.sha256()
with open(tarball, "rb") as fh:
    for chunk in iter(lambda: fh.read(1 << 20), b""):
        digest.update(chunk)

bucket = env("custom_build_upload_bucket")
remote_file = env("REMOTE_FILE")
manifest_remote_file = env("MANIFEST_REMOTE_FILE")
task_id = env("task_id")
manifest = {
    "schema_version": 1,
    "delivery_channel": env("DELIVERY_CHANNEL"),
    "version": env("version"),
    "version_suffix": env("custom_build_version_suffix"),
    "git_tag": env("triggered_by_git_tag"),
    "revision": env("revision"),
    "branch": env("branch_name"),
    "project": env("project"),
    "build_variant": env("build_variant"),
    "push_arch": env("push_arch"),
    "built_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "artifact": {
        "filename": env("TARBALL_NAME"),
        "bucket": bucket,
        "key": remote_file,
        "s3_uri": f"s3://{bucket}/{remote_file}",
        "sha256": digest.hexdigest(),
        "size_bytes": os.path.getsize(tarball),
    },
    "manifest": {
        "bucket": bucket,
        "key": manifest_remote_file,
        "s3_uri": f"s3://{bucket}/{manifest_remote_file}",
    },
    "evergreen": {
        "task_id": task_id,
        "task_url": f"https://spruce.mongodb.com/task/{task_id}",
        "version_id": env("version_id"),
        "build_id": env("build_id"),
        "execution": env("execution"),
    },
}
with open("custom_build_manifest.json", "w") as fh:
    json.dump(manifest, fh, indent=2, sort_keys=True)
    fh.write("\n")
EOF

cat custom_build_manifest.json

cat <<EOT >custom_build_upload.yml
custom_build_upload_remote_file: "${remote_file}"
custom_build_manifest_remote_file: "${manifest_remote_file}"
custom_build_delivery_channel: "${delivery_channel}"
custom_build_skip_existing: "${skip_existing}"
EOT

cat custom_build_upload.yml
