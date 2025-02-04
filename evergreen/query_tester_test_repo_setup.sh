# This script is designed to clone a specific GitHub repository that contains server tests used by query_tester.
# It ensures the repository's state is set to a specific commit for consistent testing.
#
# Inputs:
#   Environment Variables:
#     repo_name: The name of the repository to clone.
#     github_token: GitHub access token for authentication.
#   Configuration File:
#     test_repos.conf: A file containing a mapping of repository names to corresponding commit hashes in the format <repo_name>:<commit_hash>.
# Outputs:
#   - A cloned repository in src/mongo/db/query/query_tester/tests/tmp_<repo_name> that is reset to the appropriate commit hash.
#   - src/mongo/db/query/query_tester/tests/<repo_name>/generated_tests/[test_dirs] that contains unpopulated test dirs for resmoke
#     to determine the roots. These test_dirs will get populated by the query_tester_sparse_checkout.sh script.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

# Ensure that repo_name has been defined in evergreen tasks.
if [ -z "${repo_name}" ]; then
  echo "Error: No repository name provided."
  exit 1
fi

# tmp dir in which we run the git checkouts.
# Ensure that this naming convention is the same as
# query_tester_sparse_checkout.sh.
repo_name_local=tmp_${repo_name}

cd src/src/mongo/db/query/query_tester/tests
mkdir -p "${repo_name}/generated_tests"

# Find the commit hash we want to use for this repo from the config file.
commit_hash=$(grep "^${repo_name}:" test_repos.conf | awk -F ':' '{print $2}')

if [ -z "$commit_hash" ]; then
  echo "Error: Repository with name ${repo_name} not found in configuration file."
  exit 1
fi

for i in {1..5}; do
  git clone --sparse --filter=blob:none https://x-access-token:${github_token}@github.com/10gen/${repo_name}.git ${repo_name_local} && RET=0 && break || RET=$? && sleep 5
  echo "Failed to clone github.com:10gen/${repo_name}.git, retrying..."
done

if [ $RET -ne 0 ]; then
  echo "Failed to clone git@github.com:10gen/${repo_name}.git"
  exit $RET
fi

cd ${repo_name_local} || exit 1

git reset --hard ${commit_hash}
RESET_RET=$?
# create generated_test inside of $repo_name_local (the actual git repo)
mkdir generated_tests/

# Make sure the commit hash refers to a valid commit in the repo.
if [ $RESET_RET -ne 0 ]; then
  echo "Failed to reset to commit ${commit_hash} in ${repo_name_local}."
  exit $RESET_RET
else
  echo "Successfully reset ${repo_name_local} to commit ${commit_hash}."
fi

# Create directories for all tests before checking them out. This
# allows the suite(s) to recognize which tests are present.
git ls-tree -d --name-only HEAD generated_tests/ | xargs -I {} mkdir -p ../${repo_name}/{}
