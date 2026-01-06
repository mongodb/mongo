#!/bin/bash

set -e

# Write Endor Labs API credentials to config.yml
# Requires: AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN, ENDOR_CONFIG_PATH, ENDOR_NAMESPACE

# use AWS CLI to get the Endor Labs API credentials from AWS Secrets Manager
ENDOR_API_CREDENTIALS_KEY=$(aws secretsmanager get-secret-value --secret-id silkbomb-environment --region us-east-1 --query SecretString --output text | jq -r '.ENDOR_API_CREDENTIALS_KEY')
ENDOR_API_CREDENTIALS_SECRET=$(aws secretsmanager get-secret-value --secret-id silkbomb-environment --region us-east-1 --query SecretString --output text | jq -r '.ENDOR_API_CREDENTIALS_SECRET')

# save credentials to config file
mkdir -p ${ENDOR_CONFIG_PATH}
cat <<EOF >${ENDOR_CONFIG_PATH}/config.yaml
ENDOR_API: https://api.endorlabs.com
ENDOR_API_CREDENTIALS_KEY: $ENDOR_API_CREDENTIALS_KEY
ENDOR_API_CREDENTIALS_SECRET: $ENDOR_API_CREDENTIALS_SECRET
ENDOR_NAMESPACE: ${ENDOR_NAMESPACE}
EOF
