# Forced early return indicating success.
# Note: This is part of the migration to the GitHub queue; we're going to
# switch from the validate_commit_message approach to using GH native
# rulesets to validate commit message formatting.
exit 0

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose
set -o errexit
if [ "${is_commit_queue}" = "true" ]; then
  # Fetch the gitlog from the last merge commit to HEAD, which is guaranteed
  # to be the commit message of the PR.
  gitlog=$(git log --pretty=format:'%s' v5.0-test-merge-queue...HEAD 2>&1)
  # In case there are any special characters that could cause issues (like "). To
  # do this, we will write it out to a file, then read that file into a variable.
  cat > commit_message.txt << END_OF_COMMIT_MSG
${gitlog}
END_OF_COMMIT_MSG

  commit_message_content=$(cat commit_message.txt)

  activate_venv
  $python buildscripts/validate_commit_message.py "$commit_message_content"
fi
