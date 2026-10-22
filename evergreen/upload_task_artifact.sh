set -o errexit
set -o pipefail

# A failing invocation must not attach links left by an earlier upload on this task-group host.
printf '[]\n' >src/task-artifact.json

# Use the existing task venv directly. prelude.sh reloads expansions.yml, which may contain stale
# task-group identity or upload parameters. Use the current command's expanded environment, but
# these task-controlled expansions are not authenticated identity or an authorization boundary.
artifact_python="$PWD/venv/bin/python"
if [ "${OS:-}" = "Windows_NT" ]; then
    artifact_python="$PWD/venv/Scripts/python.exe"
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
"$artifact_python" -m buildscripts.task_artifacts \
    "--local-file=$artifact_file" \
    "--relative-name=${artifact_relative_name:?}" \
    "--display-name=${artifact_display_name:-}" \
    "--content-type=${artifact_content_type:-application/octet-stream}" \
    "--permissions=${artifact_permissions:-private}" \
    "--visibility=${artifact_visibility:-signed}" \
    "--optional=${artifact_optional:-false}"
