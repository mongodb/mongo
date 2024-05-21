# Note: This script is called "commit_message_validate", but it does not actually
# evaluate the commit message.  Rather, it leverages the fact that a GitHub merge queue
# triggered branch will have a single commit message that is the name of the PR.
# This script ensures the PR name matches a well-known regex (see below for details).
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
cd src
set -o verbose
set -o errexit
if [ "${is_commit_queue}" = "true" ]; then
  # Fetch the gitlog from the last merge commit to HEAD, which is guaranteed
  # to be the commit message of the PR.
  gitlog=$(git log --pretty=format:'%s' ${branch_name}...HEAD 2>&1)
  # In case there are any special characters that could cause issues (like "). To
  # do this, we will write it out to a file, then read that file into a variable.
  cat > commit_message.txt << END_OF_COMMIT_MSG
${gitlog}
END_OF_COMMIT_MSG
  commit_message_content=$(cat commit_message.txt)
  activate_venv
  $python buildscripts/validate_commit_message.py "$commit_message_content"
else
  echo "Not a commit queue PR, skipping commit message validation"
  exit 0
fi
