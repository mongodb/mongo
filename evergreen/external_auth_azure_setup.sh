DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
cat << EOF > $HOME/azure_e2e_config.json
{
    "tD548GwE1@outlook.com" : "${oidc_azure_test_user_account_one_secret}",
    "tD548GwE2@outlook.com" : "${oidc_azure_test_user_account_two_secret}",
    "tD548GwE3@outlook.com" : "${oidc_azure_test_user_account_three_secret}",

    "oidc_azure_client_secret_id" : "${oidc_azure_client_secret_id}",
    "oidc_azure_client_secret_val" : "${oidc_azure_client_secret_val}",
    "oidc_azure_client_id" : "${oidc_azure_client_id}",
    "oidc_azure_tenant_id" : "${oidc_azure_tenant_id}",
    "oidc_azure_subscription_id" : "${oidc_azure_subscription_id}",
    "oidc_azure_group_name" : "${oidc_azure_group_name}",
    "oidc_azure_container_app_name" : "${oidc_azure_container_app_name}",
    "oidc_azure_container_port" : "${oidc_azure_container_port}",
    "oidc_azure_api_version" : "${oidc_azure_api_version}",
    "oidc_azure_resource_name" : "${oidc_azure_resource_name}",
    "oidc_azure_object_id" : "${oidc_azure_object_id}",
    "oidc_azure_managed_identity_api_version": "${oidc_azure_managed_identity_api_version}"
}
EOF
cat << EOF > $HOME/oidc_azure_container_key
${oidc_azure_container_key}
EOF

# EVG project variables do not preserve line breaks so we store them as base64 and decode here
sed s/[[:space:]]//g $HOME/oidc_azure_container_key | base64 --decode > $HOME/azure_remote_key

# Clean up temp file
rm -f $HOME/oidc_azure_container_key

# SSH will complain and fail if the private key permissions are too lenient (by default it is created with 644), so modify to run the test
chmod 600 $HOME/azure_remote_key

# Log some basic information about our SSH version and the final permissions and user/group of the private key file for debugging
ssh -V
ls -al $HOME/azure_remote_key

# This script enables ingress on the Azure Container App instance that we will use to obtain our managed identity token,
# restrict ingress to the local, publicly-facing IP of the host we are running on, and then output the hostname of the container app into a local file
# so that it can be dynamically consumed by subsequent test steps (such as get_token.py)
python src/mongo/db/modules/enterprise/jstests/external_auth_oidc_azure/lib/toggle_ingress.py enable --config_file=$HOME/azure_e2e_config.json --lock_file=/tmp/azure_oidc.lock | tee $HOME/azure_remote_host
