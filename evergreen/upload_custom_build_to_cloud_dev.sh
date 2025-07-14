# see SERVER-107057 for information about this script

if [ "$PROMOTE_TO_CLOUD_DEV" = '' ]; then
  echo "Skipping promotion to cloud-dev"
  exit 0
fi

case "$PROMOTE_BUILD_VARIANT" in
"enterprise-amazon2" | "amazon2-x86-compile")
  promote_arch="amd64"
  promote_flavor="amazon2"
  promote_min_os_version="2"
  ;;
"enterprise-amazon2-arm64" | "amazon2-arm64-compile")
  promote_arch="aarch64"
  promote_flavor="amazon2"
  promote_min_os_version="2"
  ;;
"enterprise-amazon2023" | "amazon2023-x86-compile")
  promote_arch="amd64"
  promote_flavor="amazon2023"
  promote_min_os_version=""
  ;;
"enterprise-amazon2023-arm64" | "amazon2023-arm64-compile")
  promote_arch="aarch64"
  promote_flavor="amazon2023"
  promote_min_os_version=""
  ;;
*)
  echo "Build variant ${PROMOTE_BUILD_VARIANT} cannot be used in cloud-dev, skipping"
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

echo "$body" > ./body.json

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
  -u "${CLOUD_DEV_API_PUBLIC_KEY}:${CLOUD_DEV_API_PRIVATE_KEY}" \
  https://cloud-dev.mongodb.com/api/private/nds/customMongoDbBuild)

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
