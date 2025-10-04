set -o errexit

curl --fail-with-body \
    --header "Api-User: ${EVERGREEN_API_USER}" \
    --header "Api-Key: ${EVERGREEN_API_KEY}" \
    -L https://evergreen.mongodb.com/rest/v2/tasks/${PROMOTE_TASK_ID} \
    --output ./task_data.json

echo ".................."
echo "archive_dist_test task data"
echo ".................."

cat task_data.json

fetch_address=$(cat task_data.json | jq -r '.artifacts[] | select(.name == "Binaries") | .url')

if [[ "$fetch_address" =~ ".zip" ]]; then
    promote_extension="zip"
else
    promote_extension="tgz"
fi

if [ -z "$PROMOTE_PROJECT_IDENTIFIER" ]; then
    promote_project_identifier=$(cat task_data.json | jq -r ".project_identifier")
else
    promote_project_identifier=$PROMOTE_PROJECT_IDENTIFIER
fi

promote_version_id=$(cat task_data.json | jq -r ".version_id")
promote_build_id=$(cat task_data.json | jq -r ".build_id")
promote_build_variant=$(cat task_data.json | jq -r ".build_variant")
promote_revision=$(cat task_data.json | jq -r ".revision")
artifact_address="https://internal-downloads.mongodb.com/${CDN_PATH}/${promote_project_identifier}/${promote_version_id}/${promote_build_variant}/${promote_build_id}/mongo-binaries.${promote_extension}"

cat <<EOT >./promote-expansions.yml
promote_project_identifier: "$promote_project_identifier"
promote_version_id: "$promote_version_id"
promote_build_id: "$promote_build_id"
promote_build_variant: "$promote_build_variant"
promote_revision: "$promote_revision"
promote_cdn_address: "$artifact_address"
promote_extension: "$promote_extension"
EOT

echo ".................."
echo "archive_dist_test promote expansions"
echo ".................."

cat ./promote-expansions.yml

echo ""
echo "The artifact will be accessible at '$artifact_address'"
echo ""

echo "fetching artifact from $fetch_address"

curl --fail-with-body -L $fetch_address --output "mongo-binaries.$promote_extension"

attach_body=$(printf '[
    {
        "name": "Custom Build URL",
        "link": "%s",
        "visibility": "public"
    }
]' "$artifact_address")

echo "$attach_body" >./attach-address.json
