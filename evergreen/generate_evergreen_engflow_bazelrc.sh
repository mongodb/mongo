DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

source ./evergreen/bazel_RBE_supported.sh

if bazel_rbe_supported; then

  uri="https://spruce.mongodb.com/task/${task_id:?}?execution=${execution:?}"

  echo "build --tls_client_certificate=./engflow.cert" > .bazelrc.evergreen_engflow_creds
  echo "build --tls_client_key=./engflow.key" >> .bazelrc.evergreen_engflow_creds
  echo "build --bes_keywords=engflow:CiCdPipelineName=${build_variant:?}" >> .bazelrc.evergreen_engflow_creds
  echo "build --bes_keywords=engflow:CiCdJobName=${task_name:?}" >> .bazelrc.evergreen_engflow_creds
  echo "build --bes_keywords=engflow:CiCdUri=${uri:?}" >> .bazelrc.evergreen_engflow_creds
  echo "build --bes_keywords=evg:project=${project:?}" >> .bazelrc.evergreen_engflow_creds
  echo "build --remote_upload_local_results=True" >> .bazelrc.evergreen_engflow_creds
  echo "build --workspace_status_command=./evergreen/engflow_workspace_status.sh" >> .bazelrc.evergreen_engflow_creds
fi
