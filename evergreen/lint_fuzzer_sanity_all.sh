DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o pipefail
set -o verbose

LOCAL_INPUT_DIR="$PWD"
CONTAINER_INPUT_DIR="/app"

# Pull first which will do the install then allow parallel processing below
podman pull 901841024863.dkr.ecr.us-east-1.amazonaws.com/mongodb-internal/jstestfuzz:latest
# Run parse-jsfiles on 50 files at a time with 32 processes in parallel.
# Skip javascript files in third_party directory
find "jstests" "src/mongo/db/modules/enterprise" -path "$PWD/jstests/third_party" -prune -o -name "*.js" -print |
    xargs -P 32 -L 50 $CONTAINER_RUNTIME run -v $LOCAL_INPUT_DIR/jstests:$CONTAINER_INPUT_DIR/jstests -v $LOCAL_INPUT_DIR/src/mongo/db/modules/enterprise:$CONTAINER_INPUT_DIR/src/mongo/db/modules/enterprise 901841024863.dkr.ecr.us-east-1.amazonaws.com/mongodb-internal/jstestfuzz:latest npm run-script parse-jsfiles -- 2>&1 |
    tee lint_fuzzer_sanity.log
exit_code=$?

activate_venv
$python ./buildscripts/simple_report.py --test-name lint_fuzzer_sanity_all --log-file lint_fuzzer_sanity.log --exit-code $exit_code
exit $exit_code
