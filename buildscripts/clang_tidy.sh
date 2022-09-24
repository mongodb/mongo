set -o errexit
set -o verbose

CLANG_TIDY_TOOLCHAIN_VERSION="${1:-v3}"
CLANG_TIDY_FIX_MODE="${2:-scan}"

# check the version the user request matches the compile_commands
TEST_COMMAND="$(jq -r '.[] | .command' compile_commands.json | head -n 1)"
if [[ "$CLANG_TIDY_TOOLCHAIN_VERSION" != *"-force" ]] && [[ $TEST_COMMAND != "/opt/mongodbtoolchain/$CLANG_TIDY_TOOLCHAIN_VERSION"* ]]; then
  echo "ERROR: compile commands generated with different toolchain version than $CLANG_TIDY_TOOLCHAIN_VERSION"
  echo "Run with $CLANG_TIDY_TOOLCHAIN_VERSION-force to run clang-tidy anyways."
  exit 1
fi

# if they forced it, extract the raw toolchain version
if [[ "$CLANG_TIDY_TOOLCHAIN_VERSION" == *"-force" ]]; then
  # the ?????? here strips off the "-force" but character counting
  CLANG_TIDY_TOOLCHAIN_VERSION=${CLANG_TIDY_TOOLCHAIN_VERSION%??????}
fi

if [ "$CLANG_TIDY_FIX_MODE" == "fix" ]; then
  CLANG_TIDY_MAX_ARGS=1
  CLANG_TIDY_MAX_PROCESSES=1
  CLANG_TIDY_FIX_MODE="--fix-errors"
else
  CLANG_TIDY_MAX_ARGS=32
  CLANG_TIDY_MAX_PROCESSES=$(grep -c ^processor /proc/cpuinfo)
  CLANG_TIDY_FIX_MODE=""
fi

# TODO SERVER-49884 Remove this when we no longer check in generated Bison.
BISON_GENERATED_PATTERN=parser_gen\.cpp

# Here we use the -header-filter option to instruct clang-tidy to scan our header files. The
# regex instructs clang-tidy to scan headers in our source directory with the mongo/* regex, and
# the build directory to analyze generated headers with the build/* regex
jq -r '.[] | .file' compile_commands.json \
  | grep src/mongo \
  | grep -v $BISON_GENERATED_PATTERN \
  | xargs -n $CLANG_TIDY_MAX_ARGS -P $CLANG_TIDY_MAX_PROCESSES -t \
    /opt/mongodbtoolchain/$CLANG_TIDY_TOOLCHAIN_VERSION/bin/clang-tidy \
    $CLANG_TIDY_FIX_MODE -p ./compile_commands.json
