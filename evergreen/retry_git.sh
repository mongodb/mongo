#!/usr/bin/env bash
#
# Description:
#   This executes a git command with a retry. Helpful to retry commands
#   like fetch in cases where there is flaky networking.
#
# Usage:
#   retry_git.sh <args...>
#

REALGIT=$(which git)

RETRIES=5
DELAY=2
COUNT=0
while [ $COUNT -lt $RETRIES ]; do
    "$REALGIT" "$@"
    CODE=$?
    if [ $CODE -eq 0 ]; then
        RETRIES=0
        break
    fi
    ((COUNT = COUNT + 1))
    if [ $COUNT -lt $RETRIES ]; then
        echo "Git command failed, retrying in $DELAY seconds..." >&2
    fi
    sleep $DELAY
done

exit "$CODE"
