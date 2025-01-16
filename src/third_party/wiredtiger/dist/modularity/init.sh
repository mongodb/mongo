#!/bin/bash

set -ueo pipefail

[[ -f .venv/bin/activate ]] || virtualenv -q -p python3 .venv
chmod 755 .venv/bin/activate
. .venv/bin/activate

REPO_URL="https://github.com/wiredtiger/layercparse.git"
BRANCH="main"
HASH_FILE=".venv/layercparse_hash"

get_remote_hash() {
    git ls-remote $REPO_URL $BRANCH | awk '{print $1}'
}

is_layercparse_cached() {
  pip3 -qq show layercparse > /dev/null 2>&1
}

# Function to check if layercparse cache is older than 24 hours
is_layercparse_cache_outdated() {
    [[ -z $(find "$HASH_FILE" -mtime -1 2>/dev/null) ]]
}

want_update() {
    ! is_layercparse_cached && REMOTE_HASH=$(get_remote_hash) && return 0
    
    if is_layercparse_cache_outdated; then
        touch "$HASH_FILE"
        local cached_hash=$(cat "$HASH_FILE")
        REMOTE_HASH=$(get_remote_hash)
        [[ -n "$REMOTE_HASH" && "$cached_hash" != "$REMOTE_HASH" ]] && return 0
    fi
    return 1
}

if want_update; then
    # Force reinstall `layercparse` to ensure the latest changes are applied, 
    # as `pip install` does not update automatically for new commits.
    pip3 -q --disable-pip-version-check install --force-reinstall git+"$REPO_URL@$BRANCH"
    echo "$REMOTE_HASH" > "$HASH_FILE"
fi
