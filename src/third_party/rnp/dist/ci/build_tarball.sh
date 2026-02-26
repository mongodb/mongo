#!/usr/bin/env bash
set -euxo pipefail

# Manually build source tarball.
# It may be run via the following command from the folder which contains this script (./rnp/ci-legacy for now):
# docker run -it -v $(pwd):/opt/scripts:ro -v /tmp/rnp-artifacts:/opt/artifacts ghcr.io/rnpgp/ci-rnp-fedora-38-amd64 bash
#
# .. and then typing /opt/scripts/build_tarball.sh v0.17.1

# Paths to cleanup from the RNP repository.
declare -a CLEAN_PATHS=(
    ".cirrus.yml"
    ".clang-format"
    ".codespellrc"
    ".editorconfig"
    ".git"
    ".gitattributes"
    ".github"
    ".gitignore"
    ".gitmodules"
    ".config.yml"
    "_config.yml"
    "ci"
    "ci-legacy"
    "codecov.yml"
    "git-hooks"
    "src/libsexpp/.git"
    "src/libsexpp/.gitattributes"
    "src/libsexpp/.github"
    "src/libsexpp/codecov.yml"
    "src/libsexpp/.gitignore"
    "src/libsexpp/.clangformat"
)

is_version() {
    [[ "$1" =~ ^v[0-9]+(\.[0-9]+)*$ ]]
}

# Check whether artifacts dir exists
RNP_ART="/opt/artifacts"
if [ ! -d "${RNP_ART}" ]; then
    >&2 echo "Error: artifacts dir ${RNP_ART} doesn't exist. Create or mount it before running the script."
    exit 1
fi

# Get the branch or tag name, main by default and setup paths.
RNP_URL="https://github.com/rnpgp/rnp.git"
RNP_REF=${1:-main}
RNP_PATH="/opt/rnp-${RNP_REF}"

# Clone
echo "Cloning ref ${RNP_REF} to ${RNP_PATH}..."
git clone --branch "${RNP_REF}" --depth 1 --single-branch --recurse-submodules "${RNP_URL}" "${RNP_PATH}"

# Cleanup files which are not needed in the tarball
for path in "${CLEAN_PATHS[@]}"; do
    echo "Removing ${RNP_PATH}/${path}..."
    rm -rf "${RNP_PATH:?}/${path}"
done

# Create tarball

RNP_BLD="/opt/rnp-build"
rm -rf "${RNP_BLD}"
cmake -B "${RNP_BLD}" -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF "${RNP_PATH}"
cpack -B "${RNP_BLD}" -G TGZ --config "${RNP_BLD}/CPackSourceConfig.cmake"

# Check whether tarball builds
# cpack would use semantic versioning for file names, i.e. rnp-v0.17.1
RNP_TGZ=$(find "${RNP_BLD}" -maxdepth 1 -type f -name '*.tar.gz' | head -n 1)
RNP_CHK="/opt/rnp-check"
rm -rf "${RNP_CHK}"
mkdir -p "${RNP_CHK}"

tar -xzf "${RNP_TGZ}" -C "${RNP_CHK}"
RNP_UNP=$(find "${RNP_CHK}" -mindepth 1 -maxdepth 1 -type d | head -n 1)
cmake -B "${RNP_CHK}"/build -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=ON "${RNP_UNP}"
cmake --build "${RNP_CHK}"/build --parallel "$(nproc)"

# Copy artifacts to the /opt/artifacts
cp "${RNP_TGZ}" "${RNP_ART}"

# Calculate sha256 sums
pushd "${RNP_ART}"
RNP_SHA="$(basename "${RNP_TGZ%.tar.gz}.sha256")"
sha256sum "$(basename "${RNP_TGZ}")" > "${RNP_SHA}"
popd
