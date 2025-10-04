# see SERVER-107057 for information about this script

cloud_env="${CLOUD_ENV:-'cloud-dev'}"

if [ "$PROMOTE_TO_CLOUD_ENV" = '' ]; then
    echo "Skipping promotion to $cloud_env"
    exit 0
fi

echo "promoting build to environment '$cloud_env'"

case $PROMOTE_BUILD_VARIANT in
*"arm64"* | *"aarch64"*)
    promote_arch="aarch64"
    ;;
*"x86"* | *"amd64"*)
    promote_arch="amd64"
    ;;
*)
    echo "Could not parse architecture for build variant ${PROMOTE_BUILD_VARIANT} skipping promotion to cloud environment"
    exit 0
    ;;
esac

case $PROMOTE_BUILD_VARIANT in
*"amazon"*) ;;
*)
    echo "buildvariant ${PROMOTE_BUILD_VARIANT} doesn't appear to be an amazon buildvariant, skipping promotion to cloud environment"
    exit 0
    ;;
esac

case $PROMOTE_BUILD_VARIANT in
*"2023"*)
    promote_flavor="amazon2023"
    promote_min_os_version=""
    ;;
*"2"*)
    promote_flavor="amazon2"
    promote_min_os_version="2"
    ;;
*)
    echo "Could not parse flavor for build variant ${PROMOTE_BUILD_VARIANT} skipping promotion to cloud environment"
    exit 0
    ;;
esac

body=$(printf '{
  "trueName": "%s",
  "gitVersion": "%s",
  "architecture": "%s",
  "modules": ["enterprise"],
  "platform": "linux",
  "flavor": "%s",
  "minOsVersion": "%s",
  "url": "%s"
}' "$PROMOTE_MONGO_VERSION" "$PROMOTE_REVISION" "$promote_arch" "$promote_flavor" "$promote_min_os_version" "$PROMOTE_CDN_ADDRESS")

echo "$body" >./body.json

echo ".................."
echo "custom build endpoint body"
echo ".................."

cat ./body.json

echo "uploading custom build"

response=$(curl -sS --fail-with-body \
    -X "POST" \
    --digest \
    --header "Content-Type: application/json" \
    --data @body.json \
    -u "${CLOUD_ENV_API_PUBLIC_KEY}:${CLOUD_ENV_API_PRIVATE_KEY}" \
    "https://${cloud_env}.mongodb.com/api/private/nds/customMongoDbBuild")

result=$?

echo $response | jq

if [ $result -eq 0 ]; then
    echo "successful custom build upload"
    exit 0
else
    if [[ $response == *"DUPLICATE_MONGODB_BUILD_NAME"* ]]; then
        echo "trueName already exists, skipping upload"
        exit 0
    fi

    echo "failed to upload"
    exit 1
fi
