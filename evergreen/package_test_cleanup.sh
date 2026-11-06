#!/usr/bin/env bash
set -euo pipefail

# Run without the Python venv or prelude: timeout/post cleanup must survive early failures.
if [[ ${MONGO_PACKAGE_TEST_RUNTIME:-docker} != podman || -z ${MONGO_PACKAGE_TEST_TASK_ID:-} ||
    -z ${MONGO_PACKAGE_TEST_STATE_DIR:-} ]]; then
    exit 0
fi

# Variant-wide runtime settings also reach publishing and setup tasks. Only touch
# Podman when a package runtime actually activated cleanup for this execution.
package_test_state=$MONGO_PACKAGE_TEST_STATE_DIR
if [[ -L $package_test_state || ! -f $package_test_state/active ||
    $(<"$package_test_state/active") != "$MONGO_PACKAGE_TEST_TASK_ID" ]]; then
    exit 0
fi

# Revoke task-local registry credentials even when Podman is unavailable. Retain
# the activation marker on failure so post teardown can retry container removal.
trap 'rm -rf -- "$package_test_state/credentials"' EXIT

# Restore the exact private context published by task setup, including its pause
# namespace. Reading literal path files avoids needing the venv during teardown.
for package_test_variable in XDG_RUNTIME_DIR CONTAINERS_STORAGE_CONF CONTAINERS_CONF; do
    package_test_value=$(<"$package_test_state/$package_test_variable")
    if [[ $package_test_value != "$package_test_state/"* ]]; then
        echo "Podman $package_test_variable is outside this task's state directory" >&2
        exit 1
    fi
    export "$package_test_variable=$package_test_value"
done
export TMPDIR="$XDG_RUNTIME_DIR" TMP="$XDG_RUNTIME_DIR" TEMP="$XDG_RUNTIME_DIR"
unset REGISTRY_AUTH_FILE

# Like the shared master preflight, recover a stale pause process before removal.
if ! timeout --signal=KILL 30s podman system migrate; then
    echo "Podman migration failed; attempting package container cleanup" >&2
fi

# Never prune a shared host. The label includes the task ID and execution, so retries
# and other jobs retain their containers. Include stopped containers left by SIGKILL.
package_test_containers=$(timeout --signal=KILL 30s podman ps --all --quiet \
    --filter "label=org.mongodb.package-test.task=${MONGO_PACKAGE_TEST_TASK_ID}")
if [[ -n $package_test_containers ]]; then
    mapfile -t package_test_container_ids <<<"$package_test_containers"
    timeout --signal=KILL 30s podman rm --force --time=0 "${package_test_container_ids[@]}"
fi

# All containers and images in this storage belong to the current execution.
# Match master's overlay recovery and one reset retry when FUSE teardown races.
if ! timeout --signal=KILL 30s podman system reset --force; then
    package_test_overlay="${CONTAINERS_STORAGE_CONF%/*}/graphroot/overlay-containers"
    timeout --signal=KILL 30s podman unshare umount -R -l "$package_test_overlay" || true
    timeout --signal=KILL 30s podman system reset --force
fi
rm -rf -- "$package_test_state"
