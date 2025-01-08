# This script is designed to clone a specific GitHub repository that contains server tests used by query_tester.
# It ensures the repository's state is set to a specific commit for consistent testing. This associates each server commit with a test repository commit.
#
# Inputs:
#   Environment Variables:
#     repo_name: The name of the repository to clone.
#     github_token: GitHub access token for authentication.
#   Configuration File:
#     test_repos.conf: A file containing a mapping of repository names to corresponding commit hashes in the format <repo_name>:<commit_hash>.
# Outputs:
#   A cloned repository in src/mongo/db/query/query_tester/tests/<repo_name> that is reset to the appropriate commit hash.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

# Ensure that repo_name has been defined in evergreen tasks.
if [ -z "${repo_name}" ]; then
  echo "Error: No repository name provided."
  exit 1
fi

cd src/src/mongo/db/query/query_tester/tests

# Find the commit hash we want to use for this repo from the config file.
commit_hash=$(grep "^${repo_name}:" test_repos.conf | awk -F ':' '{print $2}')

if [ -z "$commit_hash" ]; then
  echo "Error: Repository with name ${repo_name} not found in configuration file."
  exit 1
fi

for i in {1..5}; do
  git clone https://x-access-token:${github_token}@github.com/10gen/${repo_name}.git ${repo_name} && RET=0 && break || RET=$? && sleep 5
  echo "Failed to clone github.com:10gen/${repo_name}.git, retrying..."
done

if [ $RET -ne 0 ]; then
  echo "Failed to clone git@github.com:10gen/${repo_name}.git"
  exit $RET
fi

cd ${repo_name} || exit 1

git reset --hard ${commit_hash}
RESET_RET=$?

# Make sure the commit hash refers to a valid commit in the repo.
if [ $RESET_RET -ne 0 ]; then
  echo "Failed to reset to commit ${commit_hash} in ${repo_name}."
  exit $RESET_RET
else
  echo "Successfully reset ${repo_name} to commit ${commit_hash}."
fi
