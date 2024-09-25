DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# Use the Evergreen temp directory to avoid filling up the disk.
mkdir -p $TMPDIR
if [[ "$OSTYPE" == "cygwin" ]] || [[ "$OSTYPE" == "win32" ]]; then
  # TODO(SERVER-94605): remove when Windows temp directory is cleared between task runs
  rm -rf Z:/bazel_tmp/* || true

  # Z:/ path is necessary to avoid running into MSVC's file length limit,
  # see https://jira.mongodb.org/browse/DEVPROD-11126
  abs_path=$(cygpath -w "$TMPDIR" | tr '\\' '/')
  echo "startup --output_user_root=Z:/bazel_tmp" > .bazelrc.evergreen
  echo "BAZELISK_HOME=${abs_path}/bazelisk_home" >> .bazeliskrc
else
  echo "startup --output_user_root=${TMPDIR}/bazel-output-root" > .bazelrc.evergreen
  echo "BAZELISK_HOME=${TMPDIR}/bazelisk_home" >> .bazeliskrc
fi

# Setup the EngFlow credentials for Evergreen builds if remote execution is enabled.
source ./evergreen/bazel_RBE_supported.sh

if bazel_rbe_supported; then

  uri="https://spruce.mongodb.com/task/${task_id:?}?execution=${execution:?}"

  echo "build --tls_client_certificate=./engflow.cert" >> .bazelrc.evergreen
  echo "build --tls_client_key=./engflow.key" >> .bazelrc.evergreen
  echo "build --bes_keywords=engflow:CiCdPipelineName=${build_variant:?}" >> .bazelrc.evergreen
  echo "build --bes_keywords=engflow:CiCdJobName=${task_name:?}" >> .bazelrc.evergreen
  echo "build --bes_keywords=engflow:CiCdUri=${uri:?}" >> .bazelrc.evergreen
  echo "build --bes_keywords=evg:project=${project:?}" >> .bazelrc.evergreen
  echo "build --remote_upload_local_results=True" >> .bazelrc.evergreen
  echo "build --workspace_status_command=./evergreen/engflow_workspace_status.sh" >> .bazelrc.evergreen
fi
