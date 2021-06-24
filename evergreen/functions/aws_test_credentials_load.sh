DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
echo "const AWS_KMS_SECRET_ID = '${aws_kms_access_key_id}';" >> src/mongo/db/modules/enterprise/jstests/fle/lib/aws_secrets.js
echo "const AWS_KMS_SECRET_KEY = '${aws_kms_secret_access_key}';" >> src/mongo/db/modules/enterprise/jstests/fle/lib/aws_secrets.js

echo "const KMS_GCP_EMAIL = '${kms_gcp_email}'; " >> src/mongo/db/modules/enterprise/jstests/fle/lib/aws_secrets.js
echo "const KMS_GCP_PRIVATEKEY = '${kms_gcp_privatekey}'; " >> src/mongo/db/modules/enterprise/jstests/fle/lib/aws_secrets.js

echo "const KMS_AZURE_TENANT_ID = '${kms_azure_tenant_id}';" >> src/mongo/db/modules/enterprise/jstests/fle/lib/aws_secrets.js
echo "const KMS_AZURE_CLIENT_ID = '${kms_azure_client_id}';" >> src/mongo/db/modules/enterprise/jstests/fle/lib/aws_secrets.js
echo "const KMS_AZURE_CLIENT_SECRET = '${kms_azure_client_secret}';" >> src/mongo/db/modules/enterprise/jstests/fle/lib/aws_secrets.js
