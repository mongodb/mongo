# The repos have been cloned without content blobs. This is called a
# "sparse" checkout. Here we download the blobs for tests
# individually. After checking out each test case (seed directory), we
# create a file called .sparse-checkout-done in that directory.
# This can be used externally as a signal that the test case checkout is
# complete and execution can begin.
# This test directory is then copied to the directory detected by resmoke's root selector.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

# Ensure that repo_name has been defined in evergreen tasks.
if [ -z "$1" ]; then
  echo "Error: No repository name provided."
  exit 1
fi

repo_name="$1"
# tmp dir in which we run the git checkouts.
# Ensure that this naming convention is the same as
# query_tester_repo_setup.sh.
repo_name_local=tmp_${repo_name}

cd src/src/mongo/db/query/query_tester/tests
cd $repo_name_local

ls ../${repo_name}/generated_tests | while read i; do
  TEST_DIR=generated_tests/$i
  echo "Checking out $TEST_DIR at $(date)"
  git sparse-checkout add $TEST_DIR
  touch $TEST_DIR/.sparse-checkout-done
  cp $TEST_DIR/* $TEST_DIR/.sparse-checkout-done ../$repo_name/$TEST_DIR/
done
