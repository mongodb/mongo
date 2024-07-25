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

pipx install "cpp-coveralls==0.4.2" || exit 1

# Run coveralls and upload data.
# View at https://coveralls.io/github/10gen/mongo
cpp-coveralls \
  --root ./build/debug \
  --build-root . \
  --verbose \
  --gcov ${GCOV_TOOL[@]} \
  --exclude src/third_party/
