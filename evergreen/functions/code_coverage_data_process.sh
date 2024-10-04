DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set +o errexit

cd src

ARCH=$(uname -m)
if [[ "$ARCH" == "s390x" || "$ARCH" == "s390" || "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" || "$ARCH" == "ppcle" ]]; then
  echo "Code coverage not supported on this architecture"
  exit 0
fi

# TODO (SERVER-94775): generalize the bazel command
BAZEL_BINARY=$TMPDIR/bazelisk
BAZEL_OUTPUT_PATH=$($BAZEL_BINARY info output_path)

gcno_files=$(find $BAZEL_OUTPUT_PATH -type f -name "*.gcno")
if [ -n "$gcno_files" ]; then
  echo "Found code coverage files:"
  find $BAZEL_OUTPUT_PATH -type f -name "*.gcno"

  # Use coverage-reporter to scrape and upload data left by Bazel test targets.
  # This implicitly uploads the data.
  # https://github.com/coverallsapp/coverage-reporter
  if [[ "$ARCH" == "arm64" || "$ARCH" == "aarch64" ]]; then
    # arm64
    echo "Coveralls coverage reporter is not yet supported on arm64."
    echo "https://github.com/coverallsapp/coverage-reporter/issues/137"
    # don't exit, there may be more data from SCons-produced builds
  else
    # amd64
    curl -L https://github.com/coverallsapp/coverage-reporter/releases/download/v0.6.14/coveralls-linux.tar.gz | tar -xz
    # checksum from their release artifacts
    echo 'a7e3d9d7f41a89c989a5719f52d29c84d461e0d5c07085598a2f28e4dba6f57b coveralls' | sha256sum --check

    # There are still some "travis" related namings, which get different UI affordances on the
    # Coveralls dashboard that otherwise are not shown (like the build/job links)
    ./coveralls report \
      --repo-token=${coveralls_token} \
      --pull-request=${github_pr_number} \
      --service-name="travis-ci" \
      --build-url="https://spruce.mongodb.com/version/${version_id}/" \
      --job-id=${revision_order_id} \
      --job-url="https://spruce.mongodb.com/task/${task_id}/" \
      "$BAZEL_OUTPUT_PATH/_coverage/_coverage_report.dat"
  fi
fi

# Look for evidence of instrumented artifacts
gcno_files=$(find $BAZEL_OUTPUT_PATH -type f -name "*.gcno")
if [ ! -n "$gcno_files" ]; then
  echo "No code coverage to process - no '.gcno' files found."
  exit 0
fi

echo "Found code coverage files:"
find . -type f -name "*.gcno"
find $BAZEL_OUTPUT_PATH -type f -name "*.gcno"

if [ -z "${gcov_tool}" ]; then
  echo "No coverage tool defined. Set the gcov_tool expansion in evergreen.yml" >&2
  exit 0
fi

activate_venv

# Use gcovr to process code coverage files (.gcno/.gcda) from SCons targets.
# This produces a coveralls-formatted json file that we have to explicitly upload.
# https://gcovr.com/en/stable/index.html
pipx install "gcovr==7.2" || exit 1

# gcovr relies on these env vars
export COVERALLS_REPO_TOKEN=${coveralls_token}
export TRAVIS_PULL_REQUEST=${github_pr_number}
export TRAVIS_JOB_ID=${revision_order_id}

# SCons-built tests on SCons-built src:
#   Object files found in the build/debug/ directory.
#
# SCons-built tests on Bazel-built src:
#   Object files found in the (symlink) bazel-out/ directory.
gcovr \
  --output gcovr-coveralls.json \
  --coveralls-pretty \
  --txt gcovr-coveralls.txt \
  --print-summary \
  --exclude 'build/debug/.*' \
  --exclude '.*bazel-out/.*' \
  --exclude '.*external/mongo_toolchain/.*' \
  --exclude '.*src/.*_gen\.(h|hpp|cpp)' \
  --exclude '.*src/mongo/db/cst/grammar\.yy' \
  --exclude '.*src/mongo/idl/.*' \
  --exclude '.*src/mongo/.*_test\.(h|hpp|cpp)' \
  --exclude '.*src/mongo/dbtests/.*' \
  --exclude '.*src/mongo/unittest/.*' \
  --exclude '.*/third_party/.*' \
  --gcov-ignore-errors source_not_found \
  --gcov-ignore-parse-errors negative_hits.warn \
  --gcov-exclude-directories '.*src/mongo/dbtests/.*' \
  --gcov-exclude-directories '.*src/mongo/idl/.*' \
  --gcov-exclude-directories '.*src/mongo/unittest/.*' \
  --gcov-exclude-directories '.*/third_party/.*' \
  --gcov-executable ${gcov_tool} \
  build/debug \
  $BAZEL_OUTPUT_PATH

# display tabulated stats - easy to parse and spot-check in test logs
cat gcovr-coveralls.txt

# Upload json data; view at https://coveralls.io/github/10gen/mongo
curl -X POST https://coveralls.io/api/v1/jobs -F 'json_file=@gcovr-coveralls.json'
