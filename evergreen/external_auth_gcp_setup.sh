DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src
activate_venv
set -o errexit

# Create the config file, which will contain the GCE project/zone information along with
# the expected audience that will appear on the VM's ID token.
cat <<EOF >$HOME/gce_vm_config.json
{
    "audience" : "${oidc_gcp_vm_id_token_audience}",
    "projectID" : "${oidc_gcp_project_id}",
    "zone" : "${oidc_gcp_zone}",
    "instance_template_url": "${oidc_gcp_vm_instance_template_url}"
}
EOF

# Create the SSH key file. Note that the SSH key has been base64 encoded and stored into an EVG
# environment variable, so it is first trimmed of any whitespace via sed and base64 decoded before
# being output to the file.
echo ${oidc_gcp_ssh_key} | sed "s/[[:space:]]//g" | base64 --decode >$HOME/gcp_ssh_key

# Reduce SSH keyfile privileges so that it is secure enough for OpenSSH.
chmod 600 $HOME/gcp_ssh_key

# Log some basic information about our SSH version and permissions/ownership of the private key file
# for debugging.
ssh -V
ls -al $HOME/gcp_ssh_key

# Now, create the service account private key file. The path for this file must correspond to the
# contents of GOOGLE_APPLICATION_CREDENTIALS.
# The contents of this file are expected to exist in base64 encoded format in
# $oidc_gcp_service_account_key, so the same steps are taken as above before dumping it into a
# newly-created JSON file.
echo ${oidc_gcp_service_account_key} | sed "s/[[:space:]]//g" | base64 --decode >${GOOGLE_APPLICATION_CREDENTIALS}
chmod 600 ${GOOGLE_APPLICATION_CREDENTIALS}
ls -al ${GOOGLE_APPLICATION_CREDENTIALS}

# Install google-cloud-compute so that the script can run.
$python -m pip install google-cloud-compute

# This script creates a Google Compute Engine VM instance that we will later use to obtain our managed identity token.
# It also outputs the external IP and name of the new VM into a local file so that the test knows where to SSH into
# and the teardown script knows which VM instance to delete.
$python src/mongo/db/modules/enterprise/jstests/external_auth_oidc_gcp/lib/gce_vm_manager.py create --config_file $HOME/gce_vm_config.json --service_account_key_file ${GOOGLE_APPLICATION_CREDENTIALS} --output_file $HOME/gce_vm_info.json
