DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# Use the Evergreen temp directory to avoid filling up the disk.
mkdir -p $TMPDIR
if [[ "$OSTYPE" == "cygwin" ]] || [[ "$OSTYPE" == "win32" ]]; then
    mkdir -p Z:/b
    touch Z:/b/mci_path
    # TODO(SERVER-94605): remove when Windows temp directory is cleared between task runs
    if [[ "$PWD" != "$(cat Z:/b/mci_path)" ]]; then
        echo "Clearing bazel output root from previous task mci '$(cat Z:/b/mci_path)'"
        rm -rf Z:/b/* || true
        echo $PWD >Z:/b/mci_path
    fi

    # Z:/ path is necessary to avoid running into MSVC's file length limit,
    # see https://jira.mongodb.org/browse/DEVPROD-11126
    abs_path=$(cygpath -w "$TMPDIR" | tr '\\' '/')
    echo "startup --output_user_root=Z:/b" >.bazelrc.evergreen
    echo "startup --output_base=Z:/b/b" >.bazelrc.evergreen
    echo "common --action_env=TMP=Z:/b" >>.bazelrc.evergreen
    echo "common --action_env=TEMP=Z:/b" >>.bazelrc.evergreen
    echo "BAZELISK_HOME=${abs_path}/bazelisk_home" >>.bazeliskrc
    GIT_REV=$(git rev-parse HEAD)
    echo "common --define GIT_COMMIT_HASH=${GIT_REV}" >>.bazelrc.git
else
    echo "startup --output_user_root=${TMPDIR}/bazel-output-root" >.bazelrc.evergreen
    echo "BAZELISK_HOME=${TMPDIR}/bazelisk_home" >>.bazeliskrc
    GIT_REV=$(git rev-parse HEAD)
    echo "common --define GIT_COMMIT_HASH=${GIT_REV}" >>.bazelrc.git
fi

# Size the Bazel server JVM heap as a fraction of the host's physical RAM.
# Defaults to 0.5 for all Evergreen jobs; individual tasks may override via the
# bazel_jvm_heap_ram_ratio expansion. This gives the Bazel server enough heap to
# hold many concurrent, remotely executed actions in memory (most JVMs otherwise
# default to just 25% of physical RAM). Linux only; /proc/meminfo is unavailable
# on Windows/macOS.
bazel_jvm_heap_ram_ratio="${bazel_jvm_heap_ram_ratio:-0.5}"
if [[ -n "${bazel_jvm_heap_ram_ratio}" ]] && [[ -r /proc/meminfo ]]; then
    mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
    if [[ -n "${mem_kb}" ]]; then
        xmx_mb=$(awk -v kb="${mem_kb}" -v r="${bazel_jvm_heap_ram_ratio}" 'BEGIN {printf "%d", kb / 1024 * r}')
        echo "startup --host_jvm_args=-Xmx${xmx_mb}m" >>.bazelrc.evergreen
    fi
fi

if [[ "${requester}" == "commit" ]]; then
    mongo_version=$(awk -F'MONGO_VERSION=' '/MONGO_VERSION=/ { split($2, version, /[[:space:]]/); print version[1]; exit }' .bazelrc.target_mongo_version)
    if [[ -z "${mongo_version}" ]]; then
        echo "Unable to extract MONGO_VERSION from .bazelrc.target_mongo_version" >&2
        exit 1
    fi
    echo "common --define MONGO_VERSION=${mongo_version}-${GIT_REV:0:8}" >>.bazelrc.git
fi

if [[ "${evergreen_remote_exec}" != "on" ]]; then
    # Temporarily disable remote exec and only use remote cache
    echo "common --remote_executor=" >>.bazelrc.evergreen
    echo "common --modify_execution_info=.*=+no-remote-exec" >>.bazelrc.evergreen
    echo "common --jobs=auto" >>.bazelrc.evergreen
fi

uri="https://spruce.mongodb.com/task/${task_id:?}?execution=${execution:?}"

echo "common --tls_client_certificate=./engflow.cert" >>.bazelrc.evergreen
echo "common --tls_client_key=./engflow.key" >>.bazelrc.evergreen
echo "common:public-release --tls_client_certificate=./.tmp/engflow_release.cert" >>.bazelrc.evergreen
echo "common:public-release --tls_client_key=./.tmp/engflow_release.key" >>.bazelrc.evergreen
echo "common --bes_keywords=engflow:CiCdPipelineName=${build_variant:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=engflow:CiCdJobName=${task_name:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=engflow:CiCdUri=${uri:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:project=${project:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:version_id=${version_id:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:trace_id=${otel_trace_id:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:requester=${requester:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:distro_id=${distro_id:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:build_id=${build_id:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:build_variant=${build_variant:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:task_id=${task_id:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:task_name=${task_name:?}" >>.bazelrc.evergreen
echo "common --bes_keywords=evg:execution=${execution:?}" >>.bazelrc.evergreen
echo "common --remote_upload_local_results=True" >>.bazelrc.evergreen
echo "common --test_output=summary" >>.bazelrc.evergreen
echo "common --color=yes" >>.bazelrc.evergreen # force ANSI colors as parsley can display them

# Disable remote execution in evergreen only since it runs on every PR, but we still
# want it to be fast on workstations
echo "coverage --config=no-remote-exec" >>.bazelrc.evergreen

if [[ -n "${bazelrc_flags:-}" ]]; then
    echo "common ${bazelrc_flags}" >>.bazelrc.evergreen
fi
