DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src
activate_venv
set -o errexit

# Only run this script for the external_auth_oidc_gcp task.
if [ "${task_name}" != "external_auth_oidc_gcp" ]; then
    exit 0
fi

echo "Cleaning up OIDC GCP test artifacts"

# Delete the GCP VM specified in gce_vm_info.json if GOOGLE_APPLICATION_CREDENTIALS is set, points
# to a file, and the GCE config and VM info files exist.
if [ ! -z "${GOOGLE_APPLICATION_CREDENTIALS}" ] &&
    [ -f "${GOOGLE_APPLICATION_CREDENTIALS}" ] &&
    [ -f "${HOME}/gce_vm_config.json" ] &&
    [ -f "${HOME}/gce_vm_info.json" ]; then
    # Install google-cloud-compute so that the script can run.
    $python -m pip install google-cloud-compute
    $python src/mongo/db/modules/enterprise/jstests/external_auth_oidc_gcp/lib/gce_vm_manager.py delete --config_file $HOME/gce_vm_config.json --service_account_key_file ${GOOGLE_APPLICATION_CREDENTIALS} --output_file $HOME/gce_vm_info.json
fi

# Clean up the SSH and service account keys if they exist.
if [ -f "${HOME}/gcp_ssh_key" ]; then
    rm -f $HOME/gcp_ssh_key
fi

if [ ! -z "${GOOGLE_APPLICATION_CREDENTIALS}" ] && [ -f "${GOOGLE_APPLICATION_CREDENTIALS}" ]; then
    rm -f ${GOOGLE_APPLICATION_CREDENTIALS}
fi
