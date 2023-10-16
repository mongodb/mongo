DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

python src/mongo/db/modules/enterprise/jstests/external_auth_oidc_azure/lib/toggle_ingress.py disable --config_file=$HOME/azure_e2e_config.json
