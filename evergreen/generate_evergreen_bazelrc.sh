DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# Use the Evergreen temp directory to avoid filling up the disk.
mkdir -p $TMPDIR
if [[ "$OSTYPE" == "cygwin" ]] || [[ "$OSTYPE" == "win32" ]]; then
  mkdir -p Z:/bazel_tmp
  touch Z:/bazel_tmp/mci_path
  # TODO(SERVER-94605): remove when Windows temp directory is cleared between task runs
  if [[ "$PWD" != "$(cat Z:/bazel_tmp/mci_path)" ]]; then
    echo "Clearing bazel output root from previous task mci '$(cat Z:/bazel_tmp/mci_path)'"
    rm -rf Z:/bazel_tmp/* || true
    echo $PWD > Z:/bazel_tmp/mci_path
  fi

  # Z:/ path is necessary to avoid running into MSVC's file length limit,
  # see https://jira.mongodb.org/browse/DEVPROD-11126
  abs_path=$(cygpath -w "$TMPDIR" | tr '\\' '/')
  echo "startup --output_user_root=Z:/bazel_tmp" > .bazelrc.evergreen
  echo "BAZELISK_HOME=${abs_path}/bazelisk_home" >> .bazeliskrc
  echo "common --define GIT_COMMIT_HASH=$(git rev-parse HEAD)" >> .bazelrc.gitinfo
else
  echo "startup --output_user_root=${TMPDIR}/bazel-output-root" > .bazelrc.evergreen
  echo "BAZELISK_HOME=${TMPDIR}/bazelisk_home" >> .bazeliskrc
  echo "common --define GIT_COMMIT_HASH=$(git rev-parse HEAD)" >> .bazelrc.gitinfo
fi

source ./evergreen/bazel_RBE_supported.sh

if bazel_rbe_supported && [[ "${evergreen_remote_exec}" != "on" ]]; then
  # Temporarily disable remote exec and only use remote cache
  echo "common --remote_executor=" >> .bazelrc.evergreen
  echo "common --modify_execution_info=.*=+no-remote-exec" >> .bazelrc.evergreen
  echo "common --jobs=auto" >> .bazelrc.evergreen
  echo "common:linux --local_resources=cpu=HOST_CPUS" >> .bazelrc.evergreen
fi

uri="https://spruce.mongodb.com/task/${task_id:?}?execution=${execution:?}"

echo "common --tls_client_certificate=./engflow.cert" >> .bazelrc.evergreen
echo "common --tls_client_key=./engflow.key" >> .bazelrc.evergreen
echo "common --bes_keywords=engflow:CiCdPipelineName=${build_variant:?}" >> .bazelrc.evergreen
echo "common --bes_keywords=engflow:CiCdJobName=${task_name:?}" >> .bazelrc.evergreen
echo "common --bes_keywords=engflow:CiCdUri=${uri:?}" >> .bazelrc.evergreen
echo "common --bes_keywords=evg:project=${project:?}" >> .bazelrc.evergreen
echo "common --remote_upload_local_results=True" >> .bazelrc.evergreen
echo "common --workspace_status_command=./evergreen/engflow_workspace_status.sh" >> .bazelrc.evergreen
