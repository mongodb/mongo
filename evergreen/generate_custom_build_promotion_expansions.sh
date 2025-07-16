set -o errexit

curl --fail-with-body \
    --header "Api-User: ${EVERGREEN_API_USER}" \
    --header "Api-Key: ${EVERGREEN_API_KEY}" \
    -L https://evergreen.mongodb.com/rest/v2/tasks/${PROMOTE_TASK_ID} \
    --output ./task_data.json

echo ".................."
echo "task data"
echo ".................."

cat task_data.json

promote_project_id=$(cat task_data.json | jq -r ".project_id")
promote_version_id=$(cat task_data.json | jq -r ".version_id")
promote_build_id=$(cat task_data.json | jq -r ".build_id")
promote_build_variant=$(cat task_data.json | jq -r ".build_variant")
promote_revision=$(cat task_data.json | jq -r ".revision")

artifact_address="https://internal-downloads.mongodb.com/server-custom-builds/${promote_project_id}/${promote_version_id}/${promote_build_variant}/mongo-${promote_build_id}.tgz"

cat <<EOT >./promote-expansions.yml
promote_project_id: "$promote_project_id"
promote_version_id: "$promote_version_id"
promote_build_id: "$promote_build_id"
promote_build_variant: "$promote_build_variant"
promote_revision: "$promote_revision"
promote_cdn_address: "$artifact_address"
EOT

echo ".................."
echo "promote expansions"
echo ".................."

cat ./promote-expansions.yml

echo ""
echo "The artifact will be accessible at '$artifact_address'"
echo ""

fetch_address=$(cat task_data.json | jq -r '.artifacts[] | select(.name == "Binaries") | .url')

echo "fetching artifact from $fetch_address"

curl --fail-with-body -L $fetch_address --output "mongo-binaries.tgz"
