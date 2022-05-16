DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit

cat << EOF > notary_env.sh
export NOTARY_TOKEN=${signing_auth_token_50}
export BARQUE_USERNAME=${barque_user}
export BARQUE_API_KEY=${barque_api_key}
EOF

echo "${signing_auth_token_50}" > signing_auth_token
