DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

REGISTRY="664315256653.dkr.ecr.us-east-1.amazonaws.com"
REPO="${streams_ecr_repo:-mongo/mongostream-testing}"
IMAGE="$REGISTRY/$REPO"
GITSHA="$github_commit"
ARCH="$packager_arch"
DISTRO="$packager_distro"
PATCH="$is_patch"

if [ "$ARCH" == "aarch64" ]; then
    ARCH="arm64"
fi

if [ "$ARCH" == "x86_64" ]; then
    ARCH="amd64"
fi

TAG_SUFFIX="$ARCH"

if [ "$DISTRO" == "amazon2023" ]; then
    TAG_SUFFIX="$ARCH-al2023"
fi

# Optional variant-specific suffix to avoid ECR tag collisions when multiple
# variants run streams_build_and_push with the same arch/distro/patch.
if [ -n "$streams_variant_tag" ]; then
    TAG_SUFFIX="$TAG_SUFFIX$streams_variant_tag"
fi

if [ "$PATCH" ]; then
    TAG_SUFFIX="$TAG_SUFFIX-$revision_order_id"
fi

for arg in "$@"; do
    if [ "$arg" == "--break-glass" ]; then
        TAG_SUFFIX="${TAG_SUFFIX}_break_glass"
        break
    fi
done

MONGOD_PATH="./src/bazel-bin/src/mongo/db/mongod"
MONGO_PATH="./src/bazel-bin/src/mongo/shell/mongo"
echo "Current mongod path: $MONGOD_PATH"
echo "Current mongo path: $MONGO_PATH"

cd src

mkdir -p ./dist-test/bin
cp -L "../$MONGOD_PATH" ./dist-test/bin/mongod
cp -L "../$MONGO_PATH" ./dist-test/bin/mongo

if [ "$DISTRO" != "amazon2023" ]; then
    echo "Skipping Docker build for distro: $DISTRO"
    echo "Creating streams-binaries.tgz with binaries only..."
    tar -czvf streams-binaries.tgz dist-test/
    echo "Created streams-binaries.tgz containing:"
    tar -tzvf streams-binaries.tgz
    exit 0
fi

attempts=0
max_attempts=4

if [ "$1" == "--push" ]; then
    while ! aws ecr get-login-password --region us-east-1 | docker login --password-stdin --username AWS $REGISTRY; do
        [ "$attempts" -ge "$max_attempts" ] && exit 1
        ((attempts++))
        sleep 10
    done
fi

mkdir -p ./bin
cp ./dist-test/bin/mongod ./bin/mongod
cp ./dist-test/bin/mongo ./bin/mongo

# Restructure asp-js-engine directory: asp-js-engine/asp-js-engine/ -> asp-js-engine/
if [ -d "asp-js-engine/asp-js-engine" ]; then
    echo "Restructuring asp-js-engine directory..."
    mv asp-js-engine/asp-js-engine asp-js-engine-temp
    rm -rf asp-js-engine
    mv asp-js-engine-temp asp-js-engine
    echo "asp-js-engine restructured successfully"
fi

# Build docker build args array
BUILD_ARGS=(--build-arg "BUILD_VERSION=$GITSHA-$TAG_SUFFIX")

# Add externalJs
BUILD_ARGS+=(--build-arg "HAS_JS_ENGINE=true")

docker build "${BUILD_ARGS[@]}" -t "$IMAGE" -f ./src/mongo/db/modules/enterprise/src/streams/build/Dockerfile.al2023 .

docker tag "$IMAGE" "$IMAGE:$GITSHA-$TAG_SUFFIX"

docker images

# Export image tag for downstream test tasks to pull from ECR
FULL_IMAGE_TAG="$IMAGE:$GITSHA-$TAG_SUFFIX"
echo "$FULL_IMAGE_TAG" >streams-image-tag.txt
echo "Exported image tag: $FULL_IMAGE_TAG"

tar -czvf streams-binaries.tgz dist-test/
echo "Created streams-binaries.tgz containing:"
tar -tzvf streams-binaries.tgz

if [ "$1" == "--push" ]; then
    docker push "$FULL_IMAGE_TAG"
fi
