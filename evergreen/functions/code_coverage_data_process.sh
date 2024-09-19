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

# Look for evidence of SCons-produced artifacts
if [ ! -d "./build/debug" ]; then
  echo "No code coverage to process - no 'build/debug' directory found."
  exit 0
fi

gcno_files=$(find ./build/debug -type f -name "*.gcno")
if [ ! -n "$gcno_files" ]; then
  echo "No code coverage to process - no '.gcno' files found."
  exit 0
fi

if [ -z "${gcov_tool}" ]; then
  echo "No coverage tool defined. Set the gcov_tool expansion in evergreen.yml" >&2
  exit 0
fi

echo "Found code coverage files:"
find ./build/debug -type f -name "*.gcno"

activate_venv

# Use gcovr to process code coverage files (.gcno/.gcda) from SCons targets.
# This produces a coveralls-formatted json file that we have to explicitly upload.
# https://gcovr.com/en/stable/index.html
pipx install "gcovr==7.2" || exit 1

# gcovr relies on these env vars
export COVERALLS_REPO_TOKEN=${coveralls_token}
export TRAVIS_PULL_REQUEST=${github_pr_number}
export TRAVIS_JOB_ID=${revision_order_id}

gcovr \
  --output gcovr-coveralls.json \
  --coveralls-pretty \
  --exclude 'build/debug/.*_gen\.(h|hpp|cpp)' \
  --exclude build/debug/mongo/db/cst/grammar.yy \
  --exclude build/debug/mongo/idl/ \
  --exclude 'src/mongo/.*_test\.(h|hpp|cpp)' \
  --exclude src/mongo/db/modules/enterprise/src/streams/third_party/ \
  --exclude src/mongo/dbtests/ \
  --exclude src/mongo/unittest/ \
  --exclude src/third_party/ \
  --gcov-ignore-errors source_not_found \
  --gcov-ignore-parse-errors negative_hits.warn \
  --gcov-exclude-directories build/debug/mongo/db/modules/enterprise/src/streams/third_party \
  --gcov-exclude-directories build/debug/mongo/dbtests/ \
  --gcov-exclude-directories build/debug/mongo/idl/ \
  --gcov-exclude-directories build/debug/mongo/unittest/ \
  --gcov-exclude-directories build/debug/third_party/ \
  --gcov-executable ${gcov_tool} \
  build/debug

# Upload json data; view at https://coveralls.io/github/10gen/mongo
curl -X POST https://coveralls.io/api/v1/jobs -F 'json_file=@gcovr-coveralls.json'
