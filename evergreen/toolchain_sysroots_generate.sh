#!/bin/bash
set +x
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
# Existing prelude helpers read optional environment variables without defaults.
set -eo pipefail
output_dir="$DIR/../toolchain-sysroot-output"
mkdir -p "$output_dir"
trap 'status=$?; printf "%s\n" "$status" > "$output_dir/exit-status"' EXIT

mode=${1:---no-push}
if [[ $mode != --publish && $mode != --no-push ]] || (($# > 1)); then
    echo "Usage: $0 [--publish|--no-push]" >&2
    exit 2
fi
if [[ ${is_patch:-false} == true ]]; then
    mode=--no-push
fi
if [[ $mode == --no-push ]]; then
    unset AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN aws_key_build aws_secret_build
fi

docker buildx version
# Match the container workflow's large CI data volume, without purging Docker state.
sudo service docker stop
sudo mkdir -p /data/mci/docker
if ! sudo jq -e . /etc/docker/daemon.json; then
    sudo sh -c 'echo "{}" > /etc/docker/daemon.json'
fi
docker_daemon_config=$(sudo jq '."data-root" = "/data/mci/docker"' /etc/docker/daemon.json)
printf '%s\n' "$docker_daemon_config" | sudo tee /etc/docker/daemon.json >/dev/null
sudo service docker start
attempt=0
until docker info >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if ((attempt >= 30)); then
        echo "Docker did not become ready after restart" >&2
        exit 1
    fi
    sleep 2
done

cd src
activate_venv
args=("$mode" --install-binfmt --output-dir=toolchain-sysroot-output)
if [[ -n ${toolchain_sysroot_distro:-} ]]; then
    args+=("--distro=$toolchain_sysroot_distro")
fi
if [[ -n ${toolchain_sysroot_architectures:-} ]]; then
    args+=("--architectures=$toolchain_sysroot_architectures")
fi
$python -m bazel.platforms.toolchain_sysroots_generator "${args[@]}"
