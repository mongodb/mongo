#!/usr/bin/env bash
#
# Usage:
#   restore_git_history_and_tags.sh
#
# Required environment variables:
# * ${dir} - target directory

cd "$dir"

set -o errexit
set -o verbose

# For older commits, evergreen will already unshallow the repository.
if [ -f .git/shallow ]; then
  # Older versions of git require this to be set, newer versions don't mind
  git config extensions.partialClone origin

  # Git versions prior to 2.20.0 do not support --filter=tree:0, so we fall
  # back to doing a blobless fetch instead.
  required_version="2.20.0"
  git_version=$(git --version | awk '{print $3}')
  if [ "$(printf '%s\n' "$required_version" "$git_version" | sort -V | head -n1)" = "$required_version" ]; then
    git fetch origin --filter=tree:0 --unshallow --tags
  else
    git fetch origin --filter=blob:none --unshallow --tags
  fi
fi
