DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

REGISTRY="664315256653.dkr.ecr.us-east-1.amazonaws.com"
REPO="mongo/mongostream"
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

# Only these 2 distros are supported today
if [ "$DISTRO" != "amazon2" ] && [ "$DISTRO" != "amazon2023" ]; then
    echo "Unsupported distro: $DISTRO" >&2
    exit 1
fi

if [ "$DISTRO" == "amazon2023" ]; then
    TAG_SUFFIX="$ARCH-al2023"
fi

if [ "$PATCH" ]; then
    TAG_SUFFIX="$TAG_SUFFIX-$revision_order_id"
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

# Build Image
MONGOD_PATH="$(find ./src -type f -name 'mongod')"
MONGO_PATH="$(find ./src -type f -name 'mongo')"
echo "Current mongod path: $MONGOD_PATH"
echo "Current mongo path: $MONGO_PATH"

mkdir -p ./src/bin
mv "$MONGOD_PATH" ./src/bin/mongod
mv "$MONGO_PATH" ./src/bin/mongo

# track asp-js-engine repo clone from evergreen modules
JS_ENGINE_PATH_ABS="$(pwd)"/src/asp-js-engine/asp-js-engine

cd src
activate_venv
setup_db_contrib_tool

# Build docker build args array
BUILD_ARGS=(--build-arg "BUILD_VERSION=$GITSHA-$TAG_SUFFIX")

# Only add JS_ENGINE_PATH build arg if the path exists
# Note: Path must be relative to the build context (src directory)
if [ -d "$JS_ENGINE_PATH_ABS" ]; then
    # Make path relative to src directory (current directory after cd src)
    JS_ENGINE_PATH_REL="asp-js-engine/asp-js-engine"
    echo "JS_ENGINE_PATH exists: $JS_ENGINE_PATH_ABS"
    echo "Using relative path for Docker: $JS_ENGINE_PATH_REL"
    BUILD_ARGS+=(--build-arg "JS_ENGINE_PATH=$JS_ENGINE_PATH_REL")
else
    echo "JS_ENGINE_PATH does not exist: $JS_ENGINE_PATH_ABS (skipping build arg)"
fi

if [ "$DISTRO" == "amazon2" ]; then
    docker build "${BUILD_ARGS[@]}" -t "$IMAGE" -f ./src/mongo/db/modules/enterprise/src/streams/build/Dockerfile .
else
    docker build "${BUILD_ARGS[@]}" -t "$IMAGE" -f ./src/mongo/db/modules/enterprise/src/streams/build/Dockerfile.al2023 .
fi

docker tag "$IMAGE" "$IMAGE:$GITSHA-$TAG_SUFFIX"

docker images

if [ "$1" == "--push" ]; then
    docker push "$IMAGE:$GITSHA-$TAG_SUFFIX"
fi
