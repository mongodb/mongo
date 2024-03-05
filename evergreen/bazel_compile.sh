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

ARCH=$(uname -m)
if [[ "$ARCH" == "arm64" || "$ARCH" == "aarch64" ]]; then
  ARCH="arm64"
elif [[ "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" || "$ARCH" == "ppcle" ]]; then
  ARCH="ppc64le"
elif [[ "$ARCH" == "s390x" || "$ARCH" == "s390" ]]; then
  ARCH="s390x"
else
  ARCH="amd64"
fi

# Set the JAVA_HOME directories for ppc64le and s390x since their bazel binaries are not compiled with a built-in JDK.
# TODO(SERVER-86050): remove the branch once bazelisk is built on s390x & ppc64le
if [[ $ARCH == "ppc64le" ]]; then
  export JAVA_HOME="/usr/lib/jvm/java-11-openjdk-11.0.4.11-2.el8.ppc64le"
elif [[ $ARCH == "s390x" ]]; then
  export JAVA_HOME="/usr/lib/jvm/java-11-openjdk-11.0.11.0.9-0.el8_3.s390x"
fi

# TODO(SERVER-86050): remove the branch once bazelisk is built on s390x & ppc64le
if [[ $ARCH == "ppc64le" ]] || [[ $ARCH == "s390x" ]]; then
  BAZEL_BINARY=$TMPDIR/bazel
else
  BAZEL_BINARY=$TMPDIR/bazelisk
fi

eval $BAZEL_BINARY build --verbose_failures $LOCAL_ARG --//bazel/config:compiler_type=${compiler} ${args} ${targets}
