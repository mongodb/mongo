# Usage:
#   bazel_compile [arguments]
#
# Required environment variables:
# * ${target} - Build target
# * ${args} - Extra command line args to pass to "bazel run"
# * ${env} - Env variable string to set (ex. ENV_VAR_ABC=123)
# * ${redact_args} - If set, redact the args in the report

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Args: ${args} Target: ${target} Env: ${env} redact_args: ${redact_args}"

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
    DISTRO=$(awk -F '[="]*' '/^PRETTY_NAME/ { print $2 }' </etc/os-release)
    if [[ $DISTRO == "Amazon Linux 2" ]]; then
        export SSL_CERT_DIR=/etc/pki/tls/certs
        export SSL_CERT_FILE=/etc/pki/tls/certs/ca-bundle.crt
    elif [[ $DISTRO == "Red Hat Enterprise Linux"* ]]; then
        export SSL_CERT_DIR=/etc/pki/ca-trust/extracted/pem
        export SSL_CERT_FILE=/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem
    fi
fi

if [[ -n "$redact_args" ]]; then
    INVOCATION_WITH_REDACTION="${target}"
else
    INVOCATION_WITH_REDACTION="${target} ${args}"
fi

# Print command being run to file that can be uploaded
echo "python buildscripts/install_bazel.py" >bazel-invocation.txt
echo "bazel run --verbose_failures ${bazel_compile_flags} ${task_compile_flags} ${LOCAL_ARG} ${INVOCATION_WITH_REDACTION}" >>bazel-invocation.txt

# Run bazel command, retrying up to five times
MAX_ATTEMPTS=5
for ((i = 1; i <= $MAX_ATTEMPTS; i++)); do
    eval $env $BAZEL_BINARY run --verbose_failures $LOCAL_ARG ${target} ${args} >>bazel_output.log 2>&1 && RET=0 && break || RET=$? && sleep 10
    if [ $i -lt $MAX_ATTEMPTS ]; then echo "Bazel failed to execute, retrying ($(($i + 1)) of $MAX_ATTEMPTS attempts)... " >>bazel_output.log 2>&1; fi
    $BAZEL_BINARY shutdown
done

$python ./buildscripts/simple_report.py --test-name "bazel run ${INVOCATION_WITH_REDACTION}" --log-file bazel_output.log --exit-code $RET
exit $RET
