DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

if command -v docker >/dev/null 2>&1; then
    echo "Docker is installed, using docker"
    CONTAINER_RUNTIME=docker

elif command -v podman >/dev/null 2>&1; then
    echo "Podman is installed, using podman"
    CONTAINER_RUNTIME=podman
else
    echo "Neither Docker nor Podman is installed. Please install one of them."
    exit 1
fi

echo "CONTAINER_RUNTIME: ${CONTAINER_RUNTIME}" >>expansions.yml

aws ecr get-login-password --region us-east-1 | $CONTAINER_RUNTIME login --username AWS --password-stdin 901841024863.dkr.ecr.us-east-1.amazonaws.com
