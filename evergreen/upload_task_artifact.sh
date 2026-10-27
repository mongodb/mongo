set -o errexit
set -o pipefail

# A failing invocation must not attach links left by an earlier upload on this task-group host.
printf '[]\n' >src/task-artifact.json

# Use the existing task venv directly. prelude.sh reloads expansions.yml, which may contain stale
# task-group identity or upload parameters. Use the current command's expanded environment, but
# these task-controlled expansions are not authenticated identity or an authorization boundary.
artifact_workdir="$PWD"
artifact_python="$PWD/venv/bin/python"
if [ "${OS:-}" = "Windows_NT" ]; then
    artifact_python="$PWD/venv/Scripts/python.exe"
    artifact_workdir=$(cygpath -wa "$artifact_workdir")
fi
# Artifact paths are relative to Evergreen's workdir, whereas the Python script runs in src.
# Resolving with cygpath also handles bazel-bin's Cygwin symlink on Windows.
artifact_file="${artifact_local_file:?}"
if [ "${OS:-}" = "Windows_NT" ]; then
    artifact_file=$(cygpath -wa "$artifact_file")
elif [[ "$artifact_file" != /* ]]; then
    artifact_file="$PWD/$artifact_file"
fi

cd src
# Python selects release policy using the variant marker, release invocation and publishing bucket.
# Provenance-bearing archives checkpoint their binary and sidecars together before public uploads.
artifact_release_inputs=()
add_release_input() {
    local name="$1" path="$2"
    artifact_release_inputs+=("--release-input=$name=$path")
}
if [[ -n "${artifact_release_provenance_file:-}${artifact_release_execution_log_file:-}${artifact_release_image_digest_file:-}${artifact_release_related_files:-}" ]]; then
    artifact_release_inputs+=("--checkpoint-release-inputs")
    if [[ -n "${artifact_release_provenance_file:-}" ]]; then
        add_release_input provenance "$artifact_release_provenance_file"
    fi
    if [[ -n "${artifact_release_execution_log_file:-}" ]]; then
        add_release_input execution-log "$artifact_release_execution_log_file"
    fi
    if [[ -n "${artifact_release_image_digest_file:-}" ]]; then
        add_release_input image-digest "$artifact_release_image_digest_file"
    fi
fi
"$artifact_python" -m buildscripts.task_artifacts \
    "--local-file=$artifact_file" \
    "--relative-name=${artifact_relative_name:?}" \
    "--display-name=${artifact_display_name:-}" \
    "--content-type=${artifact_content_type:-application/octet-stream}" \
    "--permissions=${artifact_permissions:-private}" \
    "--visibility=${artifact_visibility:-signed}" \
    "--optional=${artifact_optional:-false}" \
    "--release-input-root=$artifact_workdir" \
    "--release-related-files=${artifact_release_related_files:-[]}" \
    "${artifact_release_inputs[@]}"
