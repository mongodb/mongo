# !/bin/bash
# Run SilkBomb's `update --select-licenses` against sbom.json and sbom.private.json inside a
# container, tagging each component's license as "declared" or "concluded" (CycloneDX
# license.acknowledgement) based on MongoDB's Inbound Open Source Policy license ranking.
# Updates both files in place.
#
# Usage:
#   select_licenses.sh
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
#   CONTAINER_IMAGE
#   SBOM_REPO_ROOT

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../../prelude.sh"

set -o errexit
set -o verbose
set -o pipefail

read -ra OPTS_ARRAY <<<"$CONTAINER_OPTIONS"

echo "--> Logging in to AWS ECR..."
aws ecr get-login-password --region us-east-1 | "${CONTAINER_COMMAND}" login --username AWS --password-stdin 901841024863.dkr.ecr.us-east-1.amazonaws.com

for sbom_file in sbom.private.json sbom.json; do
    echo "--> Running the container to select licenses for ${sbom_file}..."
    # The "${VAR[@]}" syntax expands arrays safely, with each element becoming a distinct argument.
    "${CONTAINER_COMMAND}" run \
        "${OPTS_ARRAY[@]}" \
        --env-file "${CONTAINER_ENV_FILES}" \
        -v "${SBOM_REPO_ROOT}:/workdir" \
        "${CONTAINER_IMAGE}" \
        update \
        --sbom-in "/workdir/${sbom_file}" \
        --sbom-out "/workdir/${sbom_file}" \
        --select-licenses \
        --sort \
        --no-update-timestamp \
        --no-update-sbom-version
done

echo "--> Script finished successfully."
