# !/bin/bash
# Augment SBOM using SilkBomb inside a container.
#
# Usage:
#   augment_sbom
#
# The script uses SilkBomb.
# See: https://docs.devprod.prod.corp.mongodb.com/mms/python/src/sbom/silkbomb/
#
# Required system environment variables:
#   AWS_ACCESS_KEY_ID
#   AWS_SECRET_ACCESS_KEY
#   AWS_SESSION_TOKEN
#
# Required script env variables:
#   CONTAINER_COMMAND
#   CONTAINER_OPTIONS
#   CONTAINER_ENV_FILES
#   CONTAINER_VOLUMES
#   CONTAINER_IMAGE
#   SBOM_REPO_PATH
#   SBOM_OUT_PATH
#   SILKBOMB_COMMAND
#   SILKBOMB_ARGS
#   requester
#   branch_name
#   github_org
#   github_repo
#   workdir

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../../prelude.sh"

set -o errexit
set -o verbose
set -o pipefail

read -ra OPTS_ARRAY <<<"$CONTAINER_OPTIONS"
read -ra VOLUMES_ARRAY <<<"$CONTAINER_VOLUMES"
read -ra ARGS_ARRAY <<<"$SILKBOMB_ARGS"

echo "--> Logging in to AWS ECR..."
aws ecr get-login-password --region us-east-1 | "${CONTAINER_COMMAND}" login --username AWS --password-stdin 901841024863.dkr.ecr.us-east-1.amazonaws.com

echo "--> Running the container..."
# The "${VAR[@]}" syntax expands arrays safely, with each element becoming a distinct argument.
"${CONTAINER_COMMAND}" run \
    "${OPTS_ARRAY[@]}" \
    --env-file "${CONTAINER_ENV_FILES}" \
    "${VOLUMES_ARRAY[@]}" \
    "${CONTAINER_IMAGE}" \
    "${SILKBOMB_COMMAND}" \
    "${ARGS_ARRAY[@]}"

echo "--> Script finished successfully."
