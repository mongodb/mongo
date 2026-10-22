#!/bin/bash
# Executed with bash -c: $0 and $@ hold the tool command. The host prepends
# lease_seconds and termination_grace_seconds from its supervision policy.
log="$MONGO_CROSS_ACTION_OUTPUT_LOG"
/bin/mkdir -p "${log%/*}" || exit 125
action_tmp=""
cleanup_scratch() {
    if [ -n "$action_tmp" ]; then
        /bin/rm -rf "$action_tmp"
        action_tmp=""
    fi
}
trap cleanup_scratch EXIT
if [ "${MONGO_CROSS_ACTION_PRIVATE_SCRATCH:-}" = "1" ]; then
    action_tmp="$(/usr/bin/mktemp -d /tmp/mongo-action-XXXXXXXX)" || exit 125
    /bin/mkdir -p "$action_tmp/home" || exit 125
    export TMPDIR="$action_tmp" TMP="$action_tmp" TEMP="$action_tmp" HOME="$action_tmp/home"
fi
unset MONGO_CROSS_ACTION_PRIVATE_SCRATCH
set -m
"$0" "$@" >"$log" 2>&1 &
tool_pid=$!
set +m
terminate_tool() {
    trap '' INT TERM HUP
    kill -TERM -- "-$tool_pid" 2>/dev/null || true
    /bin/sleep "$termination_grace_seconds"
    kill -KILL -- "-$tool_pid" 2>/dev/null || true
    wait "$tool_pid" 2>/dev/null || true
    cleanup_scratch
}
trap 'terminate_tool; printf "125\n" >"$log.exit"; exit 125' INT TERM HUP
heartbeat=""
renewed=$SECONDS
while kill -0 "$tool_pid" 2>/dev/null; do
    current=""
    if [ -f "$log.heartbeat" ]; then
        IFS= read -r current <"$log.heartbeat" || true
    fi
    if [ -n "$current" ] && [ "$current" != "$heartbeat" ]; then
        heartbeat="$current"
        renewed=$SECONDS
    fi
    if [ -f "$log.cancel" ] || ((SECONDS - renewed >= lease_seconds)); then
        terminate_tool
        printf "125\n" >"$log.exit"
        exit 125
    fi
    /bin/sleep 0.1
done
wait "$tool_pid"
status=$?
# A tool that finishes before a background child must not leave that child in the container.
kill -KILL -- "-$tool_pid" 2>/dev/null || true
cleanup_scratch
printf '%s\n' "$status" >"$log.exit"
