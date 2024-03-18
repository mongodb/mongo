DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

# Only run this script for the external_auth_oidc_azure task.
if [ "${task_name}" != "external_auth_oidc_azure" ]; then
  exit 0
fi

echo "Cleaning up Azure OIDC test artifacts"

cd src

# Clean up the SSH keyfile, if it exists
if [ -f "${HOME}/oidc_azure_container_key" ]; then
  rm -f $HOME/oidc_azure_container_key
  echo "Cleaned up container key"
fi

python src/mongo/db/modules/enterprise/jstests/external_auth_oidc_azure/lib/toggle_ingress.py disable --config_file=$HOME/azure_e2e_config.json

# Clean up the config file, if it exists
if [ -f "${HOME}/azure_e2e_config.json" ]; then
  rm -f $HOME/azure_e2e_config.json
  echo "Cleaned up azure_e2e_config.json"
fi
