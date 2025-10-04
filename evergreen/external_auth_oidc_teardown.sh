DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

# Only run this script for the external_auth_oidc_azure task.
if [ "${task_name}" != "external_auth_oidc" ]; then
    exit 0
fi

echo "Cleaning up OIDC Okta test artifacts"

#Clean up the config file, if it exists
if [ -f "${HOME}/oidc_e2e_setup.json" ]; then
    rm -f $HOME/oidc_e2e_setup.json
    echo "Cleaned up oidc_e2e_setup.json"
fi
