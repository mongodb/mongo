DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set +o errexit

cd src

if [ ! -d "./build/debug" ]; then
  echo "No code coverage to process - no 'build/debug' directory found."
  exit 0
fi

file_list=$(find ./build/debug -type f -name "*.gcno")
if [ ! -n "$file_list" ]; then
  echo "No code coverage to process - no '.gcno' files found."
  exit 0
fi

if [ -z "${GCOV_TOOL:-}" ]; then
  echo "No coverage tool defined. Set the gcov_tool expansion in evergreen.yml" >&2
  exit 0
fi

echo "Found code coverage files:"
find ./build/debug -type f -name "*.gcno"

activate_venv

pipx install "gcovr==7.2" || exit 1

# Process code coverage files (.gcno/.gcda) directly into coveralls format
# https://gcovr.com/en/stable/index.html
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
  --gcov-executable ${GCOV_TOOL[@]} \
  build/debug

# Upload json data; view at https://coveralls.io/github/10gen/mongo
curl -X POST https://coveralls.io/api/v1/jobs -F 'json_file=@gcovr-coveralls.json'
