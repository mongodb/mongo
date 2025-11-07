set -o errexit
set -o verbose

REMOVE_CONTENTS=()
function cleanup() {
    for content in "${REMOVE_CONTENTS[@]}"; do
        rm -rf "${content}"
        echo "Removed temporary content: ${content}"
    done
}

source "$(dirname $(realpath ${BASH_SOURCE[0]}))"/prelude.sh

function run_curator() {
    local -r release_version="4e26080ba03fa83f6988be90d568ff60f69524ef"
    local -r curator_url="http://boxes.10gen.com/build/curator/curator-dist-rhel70-${release_version}.tar.gz"
    local -r curator_tgz_sha256="79b9f6258ef73c6142ae8c64fc8db34bc964b94de32cd93d70b2e782c495a828"

    if ! curl --output /dev/null --silent --head --fail "${curator_url}"; then
        echo "Curator URL is not reachable: ${curator_url}. Verify that the version exists."
        exit 1
    fi

    local -r tmp_dir=$(mktemp -d)
    REMOVE_CONTENTS+=("${tmp_dir}")
    if ! curl --silent "${curator_url}" --output "${tmp_dir}/curator.tar.gz"; then
        echo "Failed to download curator from ${curator_url}: $?"
        exit 1
    fi

    local -r sha256sum=$(sha256sum --binary "${tmp_dir}/curator.tar.gz" | cut -d ' ' -f 1)
    if [[ "${sha256sum}" != "${curator_tgz_sha256}" ]]; then
        echo "Curator tarball file checksum does not match expected value: expected ${curator_tgz_sha256}, got ${sha256sum}"
        exit 1
    fi

    if ! tar -xzf "${tmp_dir}/curator.tar.gz" -C "${tmp_dir}"; then
        echo "Failed to extract curator tarball: $?"
        exit 1
    fi

    if ! "${tmp_dir}/curator" $@; then
        echo "Curator command failed: $?"
        exit 1
    fi
}

readonly CUR_DIR="$(pwd)"
readonly packages_file="packages.tgz"

podman run \
    -v "${CUR_DIR}":"${CUR_DIR}" \
    -w "${CUR_DIR}" \
    --env-host \
    ${UPLOAD_LOCK_IMAGE} \
    -key=${version_id}/${build_id}/packages/${packages_file} -tag=task-id=${EVERGREEN_TASK_ID} ${packages_file}

pushd "src" >&/dev/null

pushd ..

function trap_exit() {
    echo "Cleaning up temporary files..."
    cleanup
    popd >&/dev/null
}
trap 'trap_exit' EXIT

source ./notary_env.sh

run_curator \
    repo submit \
    --service ${barque_url} \
    --config ./etc/repo_config.yaml \
    --distro ${packager_distro} \
    --edition ${repo_edition} \
    --version ${version} \
    --arch ${packager_arch} \
    --packages ${packages_file}
