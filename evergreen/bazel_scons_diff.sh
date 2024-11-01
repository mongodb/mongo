DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

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

bazel_binary=$TMPDIR/bazelisk
if [[ $ARCH == "ppc64le" ]] || [[ $ARCH == "s390x" ]]; then
  bazel_binary=$TMPDIR/bazel
fi

extra_args="--extra_args \""
extra_args="$extra_args ${compile_flags}"
extra_args="$extra_args --evergreen-tmp-dir=${TMPDIR}"
extra_args="$extra_args\""

activate_venv

if [[ -z "${bazel_scons_diff_targets}" ]]; then
  echo "Skipping diff run since bazel_scons_diff_targets was not set"
  exit 0
fi

# Set the JAVA_HOME directories for ppc64le and s390x since their bazel binaries are not compiled with a built-in JDK.
if [[ $ARCH == "ppc64le" ]]; then
  export JAVA_HOME="/usr/lib/jvm/java-21-openjdk-21.0.4.0.7-1.el8.ppc64le"
elif [[ $ARCH == "s390x" ]]; then
  export JAVA_HOME="/usr/lib/jvm/java-21-openjdk-21.0.4.0.7-1.el8.s390x"
fi

eval ${compile_env} $python ./buildscripts/bazel_scons_diff.py \
  --bazel_binary ${bazel_binary} \
  ${extra_args} \
  ${bazel_scons_diff_targets}
exit $?
