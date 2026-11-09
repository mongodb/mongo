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

# Run the VM deletion in a subshell so a failure (errexit) does not skip the
# key cleanup below. Errexit is disabled here because capturing a subshell's
# status with "|| status=$?" would otherwise suppress errexit inside it. The
# status is preserved so a failed teardown remains visible in Evergreen.
teardown_status=0

# Delete the GCP VM specified in gce_vm_info.json if GOOGLE_APPLICATION_CREDENTIALS is set, points
# to a file, and the GCE config and VM info files exist.
if [ ! -z "${GOOGLE_APPLICATION_CREDENTIALS}" ] &&
    [ -f "${GOOGLE_APPLICATION_CREDENTIALS}" ] &&
    [ -f "${HOME}/gce_vm_config.json" ] &&
    [ -f "${HOME}/gce_vm_info.json" ]; then
    set +o errexit
    (
        set -o errexit
        . "$DIR/external_auth_gcp_venv.sh"
        setup_gcp_venv
        "$gcp_python" -I src/mongo/db/modules/enterprise/jstests/external_auth_oidc_gcp/lib/gce_vm_manager.py delete --config_file $HOME/gce_vm_config.json --service_account_key_file ${GOOGLE_APPLICATION_CREDENTIALS} --output_file $HOME/gce_vm_info.json
    )
    teardown_status=$?
    set -o errexit
fi

# Always remove the SSH and service account keys, even when an earlier step
# failed, so the secrets never outlive the task on the CI host.
if [ -f "${HOME}/gcp_ssh_key" ]; then
    rm -f $HOME/gcp_ssh_key
fi

if [ ! -z "${GOOGLE_APPLICATION_CREDENTIALS}" ] && [ -f "${GOOGLE_APPLICATION_CREDENTIALS}" ]; then
    rm -f ${GOOGLE_APPLICATION_CREDENTIALS}
fi

exit "${teardown_status}"
