DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose
set -o errexit
if [ "${is_commit_queue}" = "true" ]; then
  # Since `commit_message` is an evergreen expansion, we need a way to ensure we
  # properly deal with any special characters that could cause issues (like "). To
  # do this, we will write it out to a file, then read that file into a variable.
  cat > commit_message.txt << END_OF_COMMIT_MSG
${commit_message}
END_OF_COMMIT_MSG

  commit_message_content=$(cat commit_message.txt)

  activate_venv
  $python buildscripts/validate_commit_message.py "$commit_message_content"
fi
