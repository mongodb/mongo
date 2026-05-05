DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o pipefail

# Sanitizer-flavoured streams_image_build_and_push.sh. Builds install-devcore
# with --config=dbg_aubsan/dbg_tsan, bundles toolchain sanitizer runtime libs
# and llvm-symbolizer, uses Dockerfile.al2023.sanitizer; no multi-arch manifest.
# Variant: streams_image_build_and_push.sh. Keep in sync.

REGISTRY="664315256653.dkr.ecr.us-east-1.amazonaws.com"
REPO="${streams_ecr_repo:-mongo/mongostream-testing}"
IMAGE="$REGISTRY/$REPO"
GITSHA="$github_commit"
ARCH="$packager_arch"
DISTRO="$packager_distro"
PATCH="$is_patch"

if [ "$DISTRO" != "amazon2023" ]; then
    echo "Sanitizer streams image build is only supported on amazon2023; got $DISTRO" >&2
    exit 1
fi
# Required so the ECR image tag can't collide with the release image.
if [ -z "$streams_variant_tag" ]; then
    echo "ERROR: streams_variant_tag must be set for sanitizer builds" >&2
    exit 1
fi

if [ "$ARCH" == "aarch64" ]; then
    ARCH="arm64"
fi

if [ "$ARCH" == "x86_64" ]; then
    ARCH="amd64"
fi

TAG_SUFFIX="$ARCH-al2023$streams_variant_tag"

if [ "$PATCH" ]; then
    TAG_SUFFIX="$TAG_SUFFIX-$revision_order_id"
fi

cd src

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

if [ ! -f "$SRC_DIR/bazel-bin/install/bin/mongod" ]; then
    cd "$workdir"
    bash "$DIR/bazel_compile.sh" >"$LOGS_DIR/bazel.log" 2>&1 &
    BAZEL_PID=$!
    cd "$SRC_DIR"
fi

cd "$workdir"
FORCE_CREATE=true bash "$DIR/functions/venv_setup.sh" >"$LOGS_DIR/venv.log" 2>&1 &
VENV_PID=$!
cd "$SRC_DIR"

if ! command -v javac >/dev/null 2>&1; then
    sudo dnf -y install java-17-amazon-corretto-devel wget unzip
fi

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

rm -rf ./dist-test
mkdir -p ./dist-test
cp -RL ./bazel-bin/install/. ./dist-test/
# bazel's install outputs are read-only; restore write perms so subsequent
# build steps can write into the tree.
chmod -R u+w ./dist-test

attempts=0
max_attempts=4

if [ "$1" == "--push" ]; then
    while ! aws ecr get-login-password --region us-east-1 | docker login --password-stdin --username AWS $REGISTRY; do
        [ "$attempts" -ge "$max_attempts" ] && exit 1
        ((attempts++))
        sleep 10
    done
fi

rm -rf ./bin ./lib ./sanlib
mkdir -p ./bin ./lib ./sanlib
cp ./dist-test/bin/mongod ./bin/mongod
cp ./dist-test/bin/mongo ./bin/mongo
if [ -d ./dist-test/lib ]; then
    cp -r ./dist-test/lib/. ./lib/
fi

# Copy sanitizer runtime libs and llvm-symbolizer from the toolchain.
TOOLCHAIN_DIR=/opt/mongodbtoolchain/v5
# clang's compiler-rt sanitizer libs (e.g. libclang_rt.tsan-x86_64.so).
shopt -s nullglob
for d in "$TOOLCHAIN_DIR"/lib/clang/*/lib/*; do
    for f in "$d"/libclang_rt.asan*.so* \
        "$d"/libclang_rt.ubsan*.so* \
        "$d"/libclang_rt.lsan*.so* \
        "$d"/libclang_rt.tsan*.so*; do
        cp -L "$f" ./sanlib/
    done
done
# gcc-style sanitizer libs (libasan.so.X etc.) — used by some toolchains.
for d in "$TOOLCHAIN_DIR/lib64" "$TOOLCHAIN_DIR/lib"; do
    for f in "$d"/libasan.so* "$d"/libubsan.so* "$d"/liblsan.so* "$d"/libtsan.so*; do
        cp -L "$f" ./sanlib/
    done
done
shopt -u nullglob

if [ -z "$(ls -A ./sanlib 2>/dev/null)" ]; then
    echo "ERROR: no sanitizer runtime libs found under $TOOLCHAIN_DIR" >&2
    exit 1
fi
echo "Bundled sanitizer runtime libs:"
ls -la ./sanlib

cp -L "$TOOLCHAIN_DIR/bin/llvm-symbolizer" ./llvm-symbolizer
chmod 755 ./llvm-symbolizer

BUILD_ARGS=(--build-arg "BUILD_VERSION=$GITSHA-$TAG_SUFFIX")
BUILD_ARGS+=(--build-arg "ASPIO_JAR_PATH=aspio.jar")
BUILD_ARGS+=(--build-arg "EXTERNALJS_PATH=externaljs")
BUILD_ARGS+=(--build-arg "MONGO_LIBS_PATH=lib")
BUILD_ARGS+=(--build-arg "SAN_RUNTIME_LIBS_PATH=sanlib")
BUILD_ARGS+=(--build-arg "LLVM_SYMBOLIZER_PATH=llvm-symbolizer")
BUILD_ARGS+=(--build-arg "LSAN_SUPPRESSIONS_PATH=etc/lsan.suppressions")
BUILD_ARGS+=(--build-arg "TSAN_SUPPRESSIONS_PATH=etc/tsan.suppressions")

docker build "${BUILD_ARGS[@]}" -t "$IMAGE" -f ./src/mongo/db/modules/enterprise/src/streams/build/Dockerfile.al2023.sanitizer .

docker tag "$IMAGE" "$IMAGE:$GITSHA-$TAG_SUFFIX"

docker images

FULL_IMAGE_TAG="$IMAGE:$GITSHA-$TAG_SUFFIX"
echo "$FULL_IMAGE_TAG" >streams-image-tag.txt
echo "Exported image tag: $FULL_IMAGE_TAG"

tar -czvf streams-binaries.tgz dist-test/
echo "Created streams-binaries.tgz containing:"
tar -tzvf streams-binaries.tgz

if [ "$1" == "--push" ]; then
    docker push "$FULL_IMAGE_TAG"
fi
