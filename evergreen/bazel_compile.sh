# Usage:
#   bazel_compile [arguments]
#
# Required environment variables:
# * ${targets} - List of build targets
# * ${compiler} - One of [clang|gcc]

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Compiler: ${compiler} Targets: ${targets}"

source ./evergreen/bazel_RBE_supported.sh

if bazel_rbe_supported; then
  LOCAL_ARG=""
else
  LOCAL_ARG="--config=local"
fi

./bazelisk build --verbose_failures $LOCAL_ARG --//bazel/config:compiler_type=${compiler} ${args} ${targets}
