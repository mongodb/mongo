#!/usr/bin/env bash
set -euo pipefail

package_test_script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
package_test_fixture=$(mktemp -d)
trap 'rm -rf "$package_test_fixture"' EXIT
export PATH="$package_test_fixture:$PATH"
export PACKAGE_TEST_FIXTURE="$package_test_fixture"

cat >"$package_test_fixture/podman" <<'MOCK_PODMAN'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$PACKAGE_TEST_FIXTURE/calls"
[[ $XDG_RUNTIME_DIR == "$MONGO_PACKAGE_TEST_STATE_DIR/runtime" ]]
[[ $CONTAINERS_STORAGE_CONF == "$MONGO_PACKAGE_TEST_STATE_DIR/storage/storage.conf" ]]
[[ $CONTAINERS_CONF == "$MONGO_PACKAGE_TEST_STATE_DIR/runtime/containers.conf" ]]
case $1 in
  system)
    case $2 in
      migrate) rm -f "$PACKAGE_TEST_FIXTURE/stale-pause" ;;
      reset)
        [[ $3 == --force ]]
        if [[ ${PACKAGE_TEST_RESET_FAIL:-false} == true ]]; then exit 125; fi
        if [[ ${PACKAGE_TEST_RESET_ONCE:-false} == true && ! -f $PACKAGE_TEST_FIXTURE/reset-tried ]]; then
          touch "$PACKAGE_TEST_FIXTURE/reset-tried"
          exit 125
        fi
        ;;
      *) exit 1 ;;
    esac
    ;;
  unshare)
    [[ $2 == umount && $3 == -R && $4 == -l ]]
    [[ $5 == "$MONGO_PACKAGE_TEST_STATE_DIR/storage/graphroot/overlay-containers" ]]
    ;;
  ps)
    [[ $2 == --all && $3 == --quiet && $4 == --filter ]]
    if [[ ${PACKAGE_TEST_PS_FAIL:-false} == true ]]; then exit 125; fi
    [[ ! -f $PACKAGE_TEST_FIXTURE/stale-pause ]]
    owner=${5#label=org.mongodb.package-test.task=}
    while read -r id label; do
      if [[ $label == "$owner" ]]; then printf '%s\n' "$id"; fi
    done < "$PACKAGE_TEST_FIXTURE/containers"
    ;;
  rm)
    [[ $2 == --force && $3 == --time=0 ]]
    if [[ ${PACKAGE_TEST_RM_FAIL:-false} == true ]]; then exit 125; fi
    shift 3
    for id in "$@"; do
      awk -v id="$id" '$1 != id' "$PACKAGE_TEST_FIXTURE/containers" > "$PACKAGE_TEST_FIXTURE/remaining"
      mv "$PACKAGE_TEST_FIXTURE/remaining" "$PACKAGE_TEST_FIXTURE/containers"
    done
    ;;
  *) exit 1 ;;
esac
MOCK_PODMAN
chmod +x "$package_test_fixture/podman"
cat >"$package_test_fixture/containers" <<'CONTAINERS'
current-running task-3
current-stopped task-3
previous-attempt task-2
other-task other-3
CONTAINERS

export MONGO_PACKAGE_TEST_TASK_ID=task-3
export MONGO_PACKAGE_TEST_STATE_DIR="$package_test_fixture/state/task-3"
prepare_package_test_state() {
    mkdir -p "$MONGO_PACKAGE_TEST_STATE_DIR/runtime" "$MONGO_PACKAGE_TEST_STATE_DIR/storage"
    printf '%s' "$MONGO_PACKAGE_TEST_STATE_DIR/runtime" >"$MONGO_PACKAGE_TEST_STATE_DIR/XDG_RUNTIME_DIR"
    printf '%s' "$MONGO_PACKAGE_TEST_STATE_DIR/storage/storage.conf" >"$MONGO_PACKAGE_TEST_STATE_DIR/CONTAINERS_STORAGE_CONF"
    printf '%s' "$MONGO_PACKAGE_TEST_STATE_DIR/runtime/containers.conf" >"$MONGO_PACKAGE_TEST_STATE_DIR/CONTAINERS_CONF"
    printf 'task-3\n' >"$MONGO_PACKAGE_TEST_STATE_DIR/active"
}
export MONGO_PACKAGE_TEST_RUNTIME=docker
bash "$package_test_script_dir/package_test_cleanup.sh"
[[ ! -f $package_test_fixture/calls ]]

export MONGO_PACKAGE_TEST_RUNTIME=podman
MONGO_PACKAGE_TEST_TASK_ID= bash "$package_test_script_dir/package_test_cleanup.sh"
[[ ! -f $package_test_fixture/calls ]]

# Unrelated tasks on Podman variants must not require a working Podman installation.
PACKAGE_TEST_PS_FAIL=true bash "$package_test_script_dir/package_test_cleanup.sh"
[[ ! -f $package_test_fixture/calls ]]

mkdir -p "$MONGO_PACKAGE_TEST_STATE_DIR/credentials/auth-current" \
    "$package_test_fixture/state/task-2/credentials/auth-previous"
prepare_package_test_state
printf 'fixture\n' >"$MONGO_PACKAGE_TEST_STATE_DIR/credentials/auth-current/auth.json"
printf 'fixture\n' >"$package_test_fixture/state/task-2/credentials/auth-previous/auth.json"

# Reject a marker from a different execution before invoking Podman or removing files.
MONGO_PACKAGE_TEST_TASK_ID=task-2 bash "$package_test_script_dir/package_test_cleanup.sh"
[[ ! -f $package_test_fixture/calls ]]
[[ -f $MONGO_PACKAGE_TEST_STATE_DIR/credentials/auth-current/auth.json ]]

touch "$package_test_fixture/stale-pause"
PACKAGE_TEST_RESET_ONCE=true bash "$package_test_script_dir/package_test_cleanup.sh"
cat >"$package_test_fixture/expected" <<'CONTAINERS'
previous-attempt task-2
other-task other-3
CONTAINERS
diff -u "$package_test_fixture/expected" "$package_test_fixture/containers"
grep -Fx 'rm --force --time=0 current-running current-stopped' "$package_test_fixture/calls"
grep -Fx "unshare umount -R -l $MONGO_PACKAGE_TEST_STATE_DIR/storage/graphroot/overlay-containers" "$package_test_fixture/calls"
[[ ! -d $MONGO_PACKAGE_TEST_STATE_DIR ]]
[[ -f $package_test_fixture/state/task-2/credentials/auth-previous/auth.json ]]

# Repeated timeout/post cleanup must be harmless when no matching containers remain.
bash "$package_test_script_dir/package_test_cleanup.sh"
diff -u "$package_test_fixture/expected" "$package_test_fixture/containers"
[[ $(grep -c '^rm ' "$package_test_fixture/calls") == 1 ]]

# A runtime failure must fail teardown instead of claiming cleanup succeeded.
mkdir -p "$MONGO_PACKAGE_TEST_STATE_DIR/credentials/auth-current"
prepare_package_test_state
printf 'fixture\n' >"$MONGO_PACKAGE_TEST_STATE_DIR/credentials/auth-current/auth.json"
if PACKAGE_TEST_PS_FAIL=true bash "$package_test_script_dir/package_test_cleanup.sh"; then
    echo "Expected failed Podman listing to fail cleanup" >&2
    exit 1
fi
[[ ! -d $MONGO_PACKAGE_TEST_STATE_DIR/credentials ]]
[[ -f $MONGO_PACKAGE_TEST_STATE_DIR/active ]]

# Failed removal also revokes credentials and leaves the marker for a later retry.
printf 'cleanup-retry task-3\n' >>"$package_test_fixture/containers"
mkdir -p "$MONGO_PACKAGE_TEST_STATE_DIR/credentials/auth-current"
printf 'fixture\n' >"$MONGO_PACKAGE_TEST_STATE_DIR/credentials/auth-current/auth.json"
if PACKAGE_TEST_RM_FAIL=true bash "$package_test_script_dir/package_test_cleanup.sh"; then
    echo "Expected failed Podman removal to fail cleanup" >&2
    exit 1
fi
[[ ! -d $MONGO_PACKAGE_TEST_STATE_DIR/credentials ]]
[[ -f $MONGO_PACKAGE_TEST_STATE_DIR/active ]]

# A reset that still fails after overlay recovery must leave task state for retry.
if PACKAGE_TEST_RESET_FAIL=true bash "$package_test_script_dir/package_test_cleanup.sh"; then
    echo "Expected failed Podman reset to fail cleanup" >&2
    exit 1
fi
[[ -f $MONGO_PACKAGE_TEST_STATE_DIR/active ]]
bash "$package_test_script_dir/package_test_cleanup.sh"
[[ ! -d $MONGO_PACKAGE_TEST_STATE_DIR ]]
diff -u "$package_test_fixture/expected" "$package_test_fixture/containers"
echo "Package-test teardown checks passed"
