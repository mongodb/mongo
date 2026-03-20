DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

REGISTRY="664315256653.dkr.ecr.us-east-1.amazonaws.com"
REPO="mongo/mongostream"
IMAGE="$REGISTRY/$REPO"
GITSHA="$github_commit"
DISTRO="$packager_distro"
PATCH="$is_patch"

attempts=0
max_attempts=4

while ! aws ecr get-login-password --region us-east-1 | docker login --password-stdin --username AWS $REGISTRY; do
    [ "$attempts" -ge "$max_attempts" ] && exit 1
    ((attempts++))
    sleep 10
done

DISTRO_SUFFIX=""

if [ "$DISTRO" == "amazon2023" ]; then
    DISTRO_SUFFIX="-al2023"
fi

if [ "$PATCH" ]; then
    DISTRO_SUFFIX="$DISTRO_SUFFIX-$revision_order_id"
fi

for arg in "$@"; do
    if [ "$arg" == "--break-glass" ]; then
        DISTRO_SUFFIX="${DISTRO_SUFFIX}_break_glass"
        break
    fi
done

# Creating the manifest.
# TODO(SERVER-120347): Re-add arm64 image once we are ready to switch to using arm64.
docker manifest create $IMAGE:$GITSHA$DISTRO_SUFFIX \
    $IMAGE:$GITSHA-amd64$DISTRO_SUFFIX
#    $IMAGE:$GITSHA-arm64$DISTRO_SUFFIX

# TODO(SERVER-120347): Re-add arm64 annotation once we are ready to switch to using arm64.
# # Annotating arm64.
# docker manifest annotate $IMAGE:$GITSHA$DISTRO_SUFFIX \
#     $IMAGE:$GITSHA-arm64$DISTRO_SUFFIX --os linux --arch arm64

# Annotating amd64.
docker manifest annotate $IMAGE:$GITSHA$DISTRO_SUFFIX \
    $IMAGE:$GITSHA-amd64$DISTRO_SUFFIX --os linux --arch amd64

# Pushing the manifest.
docker manifest push $IMAGE:$GITSHA$DISTRO_SUFFIX
