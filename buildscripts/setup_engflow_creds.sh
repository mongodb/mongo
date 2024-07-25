# Setup script intended to be run locally to extract and upload the EngFlow credentials to a virtual workstation.
# See /bazel/docs/engflow_credential_setup.md for the full setup steps.

set -o errexit
set -o verbose

REMOTE_USER=$1
REMOTE_HOST=$2
ZIP_FILE=$3

if [ -z "$REMOTE_USER" ] || [ -z "$REMOTE_HOST" ] || [ -z "$ZIP_FILE" ]; then
  echo "Usage: $0 <remote_user> <remote_host> <zip_file>"
  exit 1
fi

ssh ${REMOTE_USER}@${REMOTE_HOST} "mkdir -p ~/.engflow/creds"
scp ${ZIP_FILE} ${REMOTE_USER}@${REMOTE_HOST}:~/.engflow/creds
ssh ${REMOTE_USER}@${REMOTE_HOST} "cd ~/.engflow/creds; unzip -o engflow-mTLS.zip; rm engflow-mTLS.zip"

ssh ${REMOTE_USER}@${REMOTE_HOST} "sudo chown ${REMOTE_USER}:${REMOTE_USER} /home/${REMOTE_USER}/.engflow/creds/engflow.crt /home/${REMOTE_USER}/.engflow/creds/engflow.key"
ssh ${REMOTE_USER}@${REMOTE_HOST} "sudo chmod 600 /home/${REMOTE_USER}/.engflow/creds/engflow.crt /home/${REMOTE_USER}/.engflow/creds/engflow.key"

ssh ${REMOTE_USER}@${REMOTE_HOST} "echo \"build --tls_client_certificate=/home/${REMOTE_USER}/.engflow/creds/engflow.crt\" >> ~/.bazelrc"
ssh ${REMOTE_USER}@${REMOTE_HOST} "echo \"build --tls_client_key=/home/${REMOTE_USER}/.engflow/creds/engflow.key\" >> ~/.bazelrc"
