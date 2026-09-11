DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

setup_mongo_task_generator
activate_venv

# Optional patch params to restrict generation to a single build variant or base task,
# for faster iteration. When unset, generation proceeds normally.
generate_args=()
if [[ -n "${auto_reverter_context:-}" ]]; then
    target_variant="$(jq -r '.build_variant // empty' <<<"${auto_reverter_context}")"
    target_task="$(jq -r '.failing_task // empty' <<<"${auto_reverter_context}")"
fi
if [[ -n "${target_variant:-}" ]]; then
    generate_args+=(--target-variant "${target_variant}")
fi
if [[ -n "${target_task:-}" ]]; then
    generate_args+=(--target-task "${target_task}")
fi

RUST_BACKTRACE=full PATH=$PATH:$HOME:/ ./mongo-task-generator \
    --expansion-file ../expansions.yml \
    --evg-auth-file ./.evergreen.yml \
    --evg-project-file ${evergreen_config_file_path} \
    --generate-sub-tasks-config etc/generate_subtasks_config.yml \
    --s3-test-stats-bucket mongo-test-stats \
    --include-fully-disabled-feature-tests \
    --batch-test-discovery \
    "${generate_args[@]}" \
    $@
