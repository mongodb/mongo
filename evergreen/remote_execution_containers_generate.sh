DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

# The generator builds every remote execution container image with buildx, pushes the
# multi-arch manifests to ECR, and rewrites bazel/platforms/remote_execution_containers.bzl
# with the resulting digests.

ECR_REGISTRY="public.ecr.aws"

docker buildx version

# The images are large, so keep docker's data out of the small root partition.
sudo service docker stop
sudo mkdir -p /data/mci/docker
if ! sudo jq -e . /etc/docker/daemon.json; then
    echo "docker daemon.json did not exist or was invalid, setting it to {}"
    sudo sh -c 'echo "{}" > /etc/docker/daemon.json'
fi
MODIFIED_JSON=$(sudo jq '."data-root" |= "/data/mci/docker"' /etc/docker/daemon.json)
sudo echo "${MODIFIED_JSON}" | sudo tee /etc/docker/daemon.json
sudo service docker start

# Credentials come from the ec2.assume_role in the task definition.
trap 'docker logout $ECR_REGISTRY || true' EXIT
# ECR Public tokens are only issued from us-east-1, whatever the caller's region.
aws ecr-public get-login-password --region us-east-1 | docker login --username AWS --password-stdin $ECR_REGISTRY

cd src

distro_arg=""
if [ -n "${remote_execution_container_distro:-}" ]; then
    distro_arg="--distro=${remote_execution_container_distro}"
fi

bash bazel/remote_execution_container/repin_dockerfiles.sh
git diff -- bazel/remote_execution_container/

activate_venv

# ppc64le and s390x images are built under QEMU, which --install-binfmt sets up.
# 'y' answers the prompt confirming that docker state may be purged between distros.
echo y | $python bazel/platforms/remote_execution_containers_generator.py \
    --install-binfmt --continue-on-error $distro_arg

# The container map is loaded by bazel/bzlmod.bzl, so rewriting it changes the
# bazel_features_deps extension's bzlTransitiveDigest and MODULE.bazel.lock no longer matches.
# Refresh it here so the PR carries it and `bazel run lint` stays green. Container actions are
# disabled because this only needs the loading phase, not an RBE image.
MONGO_LINUX_CONTAINER_ACTIONS=0 bazel mod deps --lockfile_mode=refresh
