#!/bin/bash
set -eux

# Retry a command up to 6 times with exponential backoff
# Usage: retry_apt.sh "description" command [args...]

if [ $# -lt 2 ]; then
    echo "Usage: $0 <description> <command> [args...]" >&2
    exit 1
fi

description="$1"
shift

export DEBIAN_FRONTEND=noninteractive
rm -rf /var/lib/apt/lists/* || true

delay=5
for i in $(seq 1 6); do
    if "$@"; then
        rm -rf /var/lib/apt/lists/* || true
        exit 0
    fi
    echo "$description failed on attempt $i... retrying in ${delay}s" >&2
    sleep ${delay}
    delay=$((delay * 2))
done

echo "$description failed after 6 attempts" >&2
exit 1
