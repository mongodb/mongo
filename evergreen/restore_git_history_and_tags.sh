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
    # Disable commit-graph writes during fetch. The unshallow --tags fetch pulls
    # in tag objects (e.g. old WiredTiger tags) whose full commit ancestry is not
    # fetched, so a commit-graph written here would reference missing objects and
    # break later git commands.
    no_commit_graph="-c gc.writeCommitGraph=false -c fetch.writeCommitGraph=false"
    if [ "$(printf '%s\n' "$required_version" "$git_version" | sort -V | head -n1)" = "$required_version" ]; then
        $retry_git $no_commit_graph fetch origin --filter=tree:0 --unshallow --tags --quiet
    else
        $retry_git $no_commit_graph fetch origin --filter=blob:none --unshallow --tags --quiet
    fi

    # If describing the commit still fails after restoring history, refetch tags.
    # Don't use `quiet` here to show more because it's our last fallback.
    git describe 2>/dev/null || $retry_git fetch origin --tags
else
    # Sometimes the tags necessary to describe a commit don't
    # get fetched due to git version, so ensure they are.
    echo "Ensuring git can describe the commit"
    git describe 2>/dev/null || git fetch --tags
fi
