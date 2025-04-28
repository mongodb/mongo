# Usage:
#   bazel_compile [arguments]
#
# Required environment variables:
# * ${target} - Build target
# * ${args} - Extra command line args to pass to "bazel run"

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Args: ${args} Target: ${target}"

source ./evergreen/bazel_utility_functions.sh
source ./evergreen/bazel_RBE_supported.sh

if bazel_rbe_supported; then
  LOCAL_ARG=""
else
  LOCAL_ARG="--config=local"
fi

if [[ "${evergreen_remote_exec}" != "on" ]]; then
  LOCAL_ARG="--config=local"
fi

BAZEL_BINARY=$(bazel_get_binary_path)

# AL2 stores certs in a nonstandard location
if [[ -f /etc/os-release ]]; then
  DISTRO=$(awk -F '[="]*' '/^PRETTY_NAME/ { print $2 }' < /etc/os-release)
  if [[ $DISTRO == "Amazon Linux 2" ]]; then
    export SSL_CERT_DIR=/etc/pki/tls/certs
    export SSL_CERT_FILE=/etc/pki/tls/certs/ca-bundle.crt
  elif [[ $DISTRO == "Red Hat Enterprise Linux"* ]]; then
    export SSL_CERT_DIR=/etc/pki/ca-trust/extracted/pem
    export SSL_CERT_FILE=/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem
  fi
fi

# Print command being run to file that can be uploaded
echo "python buildscripts/install_bazel.py" > bazel-invocation.txt
echo "bazel run --verbose_failures ${bazel_compile_flags} ${task_compile_flags} ${LOCAL_ARG} ${args} ${target}" >> bazel-invocation.txt

# Run bazel command, retrying up to five times
MAX_ATTEMPTS=5
for ((i = 1; i <= $MAX_ATTEMPTS; i++)); do
  eval $BAZEL_BINARY run --verbose_failures ${bazel_compile_flags} ${task_compile_flags} ${LOCAL_ARG} ${args} ${target}
done

# $python ./buildscripts/simple_report.py --test-name "bazel run ${args} ${target}" --log-file bazel_output.log --exit-code $RET
exit $RET
