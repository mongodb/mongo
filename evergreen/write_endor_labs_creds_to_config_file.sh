#!/bin/bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

echo "+----------------------------------------------------------------+"
echo "| Script to get Endor Labs API from Evergreen project expansions |"
echo "|                and save to endorctl config.yaml                |"
echo "+----------------------------------------------------------------+"
echo

if [[ -z "$ENDOR_CONFIG_PATH" ]]; then
    ENDOR_CONFIG_PATH = $HOME/.endorctl
fi

# use AWS CLI to get the Endor Labs API credentials from AWS Secrets Manager
ENDOR_API_CREDENTIALS_KEY=$(aws secretsmanager get-secret-value --secret-id silkbomb-environment --region us-east-1 --query SecretString --output text | jq -r '.ENDOR_API_CREDENTIALS_KEY')
ENDOR_API_CREDENTIALS_SECRET=$(aws secretsmanager get-secret-value --secret-id silkbomb-environment --region us-east-1 --query SecretString --output text | jq -r '.ENDOR_API_CREDENTIALS_SECRET')
# save credentials to config file
mkdir -p $ENDOR_CONFIG_PATH
cat <<EOF >$ENDOR_CONFIG_PATH/config.yaml
ENDOR_API: https://api.endorlabs.com
ENDOR_API_CREDENTIALS_KEY: $ENDOR_API_CREDENTIALS_KEY
ENDOR_API_CREDENTIALS_SECRET: $ENDOR_API_CREDENTIALS_SECRET
ENDOR_NAMESPACE: $ENDOR_NAMESPACE
EOF
echo "config.yaml written to $ENDOR_CONFIG_PATH"
