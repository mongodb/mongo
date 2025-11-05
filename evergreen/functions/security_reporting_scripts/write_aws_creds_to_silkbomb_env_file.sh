#!/bin/bash
#
# This is an auxiliary script that writes AWS credentials to an env file.
# Being used by augment_sbom.sh script and SBOM upload tasks to prepare the env file for SilkBomb.
#
# Usage:
#   write_aws_creds_to_silkbomb_env_file.sh
#
# Required system environment variables:
#   AWS_ACCESS_KEY_ID
#   AWS_SECRET_ACCESS_KEY
#   AWS_SESSION_TOKEN

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../../prelude.sh"

cat << EOF > "${workdir}/silkbomb.env"
AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID}
AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY}
AWS_SESSION_TOKEN=${AWS_SESSION_TOKEN}
EOF
