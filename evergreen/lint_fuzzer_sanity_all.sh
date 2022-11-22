DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jstestfuzz

set -o pipefail
set -o verbose

# Run first with help which will do the install
# Then we can run it in parallel
./src/scripts/npm_run.sh --help
# Run parse-jsfiles on 50 files at a time with 32 processes in parallel.
# Skip javascript files in third_party directory
find "$PWD/../jstests" "$PWD/../src/mongo/db/modules/enterprise" -path "$PWD/../jstests/third_party" -prune -o -name "*.js" -print | xargs -P 32 -L 50 ./src/scripts/npm_run.sh parse-jsfiles -- | tee lint_fuzzer_sanity.log
exit_code=$?

# Exit out of the jstestfuzz directory
cd ..

activate_venv
$python ./buildscripts/simple_report.py --test-name lint_fuzzer_sanity_all --log-file jstestfuzz/lint_fuzzer_sanity.log --exit-code $exit_code
exit $exit_code
