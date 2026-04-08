DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o pipefail

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

cd src

if [ "$DISTRO" != "amazon2023" ]; then
    echo "Skipping Docker build for distro: $DISTRO"
    bash "$DIR/bazel_compile.sh"
    mkdir -p ./dist-test/bin
    cp -L ./bazel-bin/src/mongo/db/mongod ./dist-test/bin/mongod
    cp -L ./bazel-bin/src/mongo/shell/mongo ./dist-test/bin/mongo
    cp -L ./bazel-bin/src/mongo/s/mongos ./dist-test/bin/mongos
    mkdir -p ./dist-test/bin/x509
    cp -L ./bazel-bin/x509/*.pem ./dist-test/bin/x509/
    tar -czvf streams-binaries.tgz dist-test/
    exit 0
fi

SRC_DIR="$(pwd)"
LOGS_DIR="${workdir}/parallel_build_logs"
mkdir -p "$LOGS_DIR"

wait_for_job() {
    local pid=$1
    local name=$2
    local log_file=$3

    if ! wait "$pid"; then
        echo "=== $name FAILED ==="
        cat "$log_file"
        exit 1
    fi
    echo "=== $name completed ==="
}

# Start bazel and venv first — they don't need the system deps.
if [ ! -f "$SRC_DIR/bazel-bin/src/mongo/db/mongod" ]; then
    cd "$workdir"
    bash "$DIR/bazel_compile.sh" >"$LOGS_DIR/bazel.log" 2>&1 &
    BAZEL_PID=$!
    cd "$SRC_DIR"
fi

cd "$workdir"
FORCE_CREATE=true bash "$DIR/functions/venv_setup.sh" >"$LOGS_DIR/venv.log" 2>&1 &
VENV_PID=$!
cd "$SRC_DIR"

# Install system deps for maven/js engine (runs in parallel with bazel/venv).
if ! command -v javac >/dev/null 2>&1; then
    sudo dnf -y install java-17-amazon-corretto-devel wget unzip
fi

# Ensure externaljs dir exists for Docker COPY even if JS engine build hasn't finished.
mkdir -p "$SRC_DIR/externaljs"

bash "$DIR/streams_build_aspio.sh" \
    --aspio-dir "$SRC_DIR/src/mongo/db/modules/enterprise/src/streams/aspio" \
    --tools-dir "$SRC_DIR/streams_build_tools" \
    --output-dir "$SRC_DIR" \
    >"$LOGS_DIR/maven.log" 2>&1 &
MAVEN_PID=$!

sudo bash "$DIR/streams_build_js_engine.sh" \
    --node-version 24 \
    --build-dir "$SRC_DIR/asp-js-engine" \
    --target-dir "$SRC_DIR/externaljs" \
    >"$LOGS_DIR/jsengine.log" 2>&1 &
JSENGINE_PID=$!

if [ -n "${BAZEL_PID:-}" ]; then
    wait_for_job $BAZEL_PID "Bazel compile" "$LOGS_DIR/bazel.log"
fi
wait_for_job $MAVEN_PID "Maven build" "$LOGS_DIR/maven.log"
wait_for_job $JSENGINE_PID "JS engine build" "$LOGS_DIR/jsengine.log"
wait_for_job $VENV_PID "Full venv setup" "$LOGS_DIR/venv.log"

mkdir -p ./dist-test/bin
cp -L ./bazel-bin/src/mongo/db/mongod ./dist-test/bin/mongod
cp -L ./bazel-bin/src/mongo/shell/mongo ./dist-test/bin/mongo
cp -L ./bazel-bin/src/mongo/s/mongos ./dist-test/bin/mongos
mkdir -p ./dist-test/bin/x509
cp -L ./bazel-bin/x509/*.pem ./dist-test/bin/x509/

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

# Build docker build args array
BUILD_ARGS=(--build-arg "BUILD_VERSION=$GITSHA-$TAG_SUFFIX")
BUILD_ARGS+=(--build-arg "ASPIO_JAR_PATH=aspio.jar")
BUILD_ARGS+=(--build-arg "EXTERNALJS_PATH=externaljs")

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

    # Only the amd64 build creates the multi-arch manifest
    if [ "$ARCH" != "amd64" ] || [ -n "${streams_variant_tag:-}" ]; then
        exit 0
    fi

    # Create and push a docker manifest so the image can be pulled without
    # specifying the architecture tag explicitly.
    MANIFEST_SUFFIX=""

    if [ "$DISTRO" == "amazon2023" ]; then
        MANIFEST_SUFFIX="-al2023"
    fi

    if [ "$PATCH" ]; then
        MANIFEST_SUFFIX="$MANIFEST_SUFFIX-$revision_order_id"
    fi

    MANIFEST_TAG="$IMAGE:$GITSHA$MANIFEST_SUFFIX"

    docker manifest create "$MANIFEST_TAG" \
        "$IMAGE:$GITSHA-amd64$MANIFEST_SUFFIX"

    docker manifest annotate "$MANIFEST_TAG" \
        "$IMAGE:$GITSHA-amd64$MANIFEST_SUFFIX" --os linux --arch amd64

    docker manifest push "$MANIFEST_TAG"
fi
