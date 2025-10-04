#!/usr/bin/env bash
#
# Usage:
#   restore_git_history_and_tags.sh
#
# Required environment variables:
# * ${dir} - target directory

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

cd "$dir"

set -o errexit
set -o verbose

retry_git="$DIR/retry_git.sh"

# For older commits, evergreen will already unshallow the repository.
if [ -f .git/shallow ]; then
    echo "Restoring git history and tags"
    # Older versions of git require this to be set, newer versions don't mind
    git config extensions.partialClone origin

    # Git versions prior to 2.20.0 do not support --filter=tree:0, so we fall
    # back to doing a blobless fetch instead.
    required_version="2.20.0"
    git_version=$(git --version | awk '{print $3}')
    if [ "$(printf '%s\n' "$required_version" "$git_version" | sort -V | head -n1)" = "$required_version" ]; then
        $retry_git fetch origin --filter=tree:0 --unshallow --tags
    else
        $retry_git fetch origin --filter=blob:none --unshallow --tags
    fi
else
    # Sometimes the tags necessary to describe a commit don't
    # get fetched due to git version, so ensure they are.
    echo "Ensuring git can describe the commit"
    git describe 2>/dev/null || git fetch --tags
fi
