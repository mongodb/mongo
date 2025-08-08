set -o errexit

curl --fail-with-body \
    --header "Api-User: ${EVERGREEN_API_USER}" \
    --header "Api-Key: ${EVERGREEN_API_KEY}" \
    -L https://evergreen.mongodb.com/rest/v2/tasks/${PROMOTE_TASK_ID} \
    --output ./debug_task_data.json

echo ".................."
echo "archive_dist_test_debug task data"
echo ".................."

cat debug_task_data.json

fetch_address=$(cat debug_task_data.json | jq -r '.artifacts[] | select(.name == "mongo-debugsymbols.tgz" or .name == "mongo-debugsymbols.zip") | .url')

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

promote_version_id=$(cat debug_task_data.json | jq -r ".version_id")
promote_build_id=$(cat debug_task_data.json | jq -r ".build_id")
promote_build_variant=$(cat debug_task_data.json | jq -r ".build_variant")
promote_archive_dist_test_task_id=$(cat debug_task_data.json | jq -r '.depends_on[0].id')

if [[ ! "$promote_archive_dist_test_task_id" =~ "archive_dist_test" ]]; then
    echo "task '$promote_archive_dist_test_task_id' does not appear to be an archive_dist_test task, please report this issue in #ask-devprod-release-tools"
    exit 1
fi

artifact_address="https://internal-downloads.mongodb.com/server-custom-builds/${promote_project_identifier}/${promote_version_id}/${promote_build_variant}/${promote_build_id}/mongo-debugsymbols.${promote_extension}"

cat <<EOT >./debug-promote-expansions.yml
promote_archive_dist_test_task_id: "$promote_archive_dist_test_task_id"
promote_cdn_address_debug: "$artifact_address"
EOT

echo ".................."
echo "archive_dist_test_debug promote expansions"
echo ".................."

cat ./debug-promote-expansions.yml

echo ""
echo "The debug symbols will be accessible at '$artifact_address'"
echo ""

echo "fetching debug symbols from $fetch_address"

curl --fail-with-body -L $fetch_address --output "mongo-debugsymbols.$promote_extension"

attach_body=$(printf '[
    {
        "name": "Custom Build Debug Symbols URL",
        "link": "%s",
        "visibility": "public"
    }
]' "$artifact_address")

echo "$attach_body" >./attach-address-debug.json
