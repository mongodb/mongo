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

while ! aws ecr get-login-password --region us-east-1 | docker login --password-stdin --username AWS $REGISTRY; do
    [ "$attempts" -ge "$max_attempts" ] && exit 1
    ((attempts++))
    sleep 10
done

# Build Image
MONGOD_PATH="$(find ./src -type f -name 'mongod')"
MONGO_PATH="$(find ./src -type f -name 'mongo')"
echo "Current mongod path: $MONGOD_PATH"
echo "Current mongo path: $MONGO_PATH"

mkdir -p ./src/bin
mv "$MONGOD_PATH" ./src/bin/mongod
mv "$MONGO_PATH" ./src/bin/mongo

cd src
activate_venv
setup_db_contrib_tool

if [ "$DISTRO" == "amazon2" ]; then
    docker build --build-arg BUILD_VERSION=$GITSHA-$TAG_SUFFIX -t "$IMAGE" -f ./src/mongo/db/modules/enterprise/src/streams/build/Dockerfile .
else
    docker build --build-arg BUILD_VERSION=$GITSHA-$TAG_SUFFIX -t "$IMAGE" -f ./src/mongo/db/modules/enterprise/src/streams/build/Dockerfile.al2023 .
fi
docker tag "$IMAGE" "$IMAGE:$GITSHA-$TAG_SUFFIX"

docker images

docker push "$IMAGE:$GITSHA-$TAG_SUFFIX"
