set -euo pipefail

# Resolve the pinned SLS image tag from the mongo repo manifest.
SLS_IMAGE_TAG=$(python3 -c "import json; print(json.load(open('src/buildscripts/modules/atlas/manifest.json'))['pinned_sls_commit'])")

if [[ ! -v disagg_storage_ecr_account ]]; then
    echo "Must set 'disagg_storage_ecr_account' project variable" >&2
    exit 1
fi

ECR_REGISTRY="${disagg_storage_ecr_account}"
ECR_SLS_REPO="${ECR_REGISTRY}/disagg-storage"

# ECR login — disagg_storage_ecr_account and the AWS_* credentials are passed in
# via include_expansions_in_env in the calling function (after ec2.assume_role).
aws ecr get-login-password --region us-east-1 |
    docker login --username AWS --password-stdin "${ECR_REGISTRY}"

# Clone the 10gen/jepsen SLS Docker-infra release tag.
# v0.4.0-evergreen-master is the first evergreen-master tag containing the SLS
# docker node + infra (SERVER-127883, PR #24).
git clone --branch=v0.4.0-evergreen-master \
    "https://x-access-token:${jepsen_github_token}@github.com/10gen/jepsen.git" jepsen

# Clone the 10gen/jepsen-io-mongodb SLS test-code release tag into the control
# image build context so it gets COPYed into the container.
git clone --branch=v0.4.0 \
    "https://x-access-token:${jepsen_io_github_token}@github.com/10gen/jepsen-io-mongodb.git" \
    jepsen/docker/control/mongodb

# Copy MongoDB binaries (mongod, mongos, mongobridge, etc.) into the
# node image build context exactly like the non-SLS setup does.
cp -rf src/dist-test jepsen/docker/node

# Extract SLS service binaries from the pinned ECR images.
# Each image's ENTRYPOINT is the service binary; we docker-create the container
# (without running it), find the entrypoint path, copy it out, then remove.
mkdir -p jepsen/docker/sls-node/dist-test/bin

extract_binary() {
    local image="$1"
    local dest_name="$2"
    local container
    container=$(docker create "${image}")
    local entrypoint
    entrypoint=$(docker inspect --format='{{index .Config.Entrypoint 0}}' "${container}")
    docker cp "${container}:${entrypoint}" \
        "jepsen/docker/sls-node/dist-test/bin/${dest_name}"
    docker rm "${container}"
}

docker pull "${ECR_SLS_REPO}/log:${SLS_IMAGE_TAG}"
docker pull "${ECR_SLS_REPO}/page:${SLS_IMAGE_TAG}"
docker pull "${ECR_SLS_REPO}/cellmetadata:${SLS_IMAGE_TAG}"
docker pull "${ECR_SLS_REPO}/pagematerializer:${SLS_IMAGE_TAG}"

extract_binary "${ECR_SLS_REPO}/log:${SLS_IMAGE_TAG}" logd
extract_binary "${ECR_SLS_REPO}/page:${SLS_IMAGE_TAG}" paged
extract_binary "${ECR_SLS_REPO}/cellmetadata:${SLS_IMAGE_TAG}" cellmetadatad
extract_binary "${ECR_SLS_REPO}/pagematerializer:${SLS_IMAGE_TAG}" pagematd

# Download grpcurl — same version and logic as fetch_images.sh.
# Checksums are the official ones from
# https://github.com/fullstorydev/grpcurl/releases/download/v1.9.1/grpcurl_1.9.1_checksums.txt
GRPCURL_VERSION="1.9.1"
case "$(uname -m)" in
x86_64)
    GRPCURL_ARCH=linux_x86_64
    GRPCURL_SHA256=588c9c429476d9ed66cd3b2ae32283a6da36e0cfbb7e446f5d6a1b68dc770214
    ;;
aarch64)
    GRPCURL_ARCH=linux_arm64
    GRPCURL_SHA256=fc0d0453dd9f276fa2158f34ba1666f7fd4d6e4053f781d0945226ebe8914cb1
    ;;
*)
    echo "unsupported arch: $(uname -m)" >&2
    exit 1
    ;;
esac
GRPCURL_URL="https://github.com/fullstorydev/grpcurl/releases/download/v${GRPCURL_VERSION}/grpcurl_${GRPCURL_VERSION}_${GRPCURL_ARCH}.tar.gz"
curl -L -sSf --retry 5 -o /tmp/grpcurl.tar.gz "${GRPCURL_URL}"
# Verify the download before trusting it; set -e aborts the run on mismatch.
echo "${GRPCURL_SHA256}  /tmp/grpcurl.tar.gz" | sha256sum -c -
tar -xz --no-same-owner -f /tmp/grpcurl.tar.gz -C jepsen/docker/sls-node/dist-test/bin/ grpcurl
rm -f /tmp/grpcurl.tar.gz

# Build images sequentially to avoid running out of disk space.
# The template docker-compose.yml uses `image: jepsen-node` / `image: jepsen-sls-node`
# so docker-compose will reuse these pre-built images rather than rebuilding per node.
sudo docker build -t jepsen-node jepsen/docker/node
sudo docker build -t jepsen-sls-node jepsen/docker/sls-node

# Kill any containers left over from a previous run.
sudo docker container kill $(docker ps -q) || true
