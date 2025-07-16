set -o errexit
set -o verbose

source "$(dirname $(realpath ${BASH_SOURCE[0]}))"/prelude.sh

activate_venv
[[ "${has_packages}" != "true" ]] && exit 0

if [[ -z "${packager_script+x}" ]]; then
    echo "Error: packager run when packager_script is not set, please remove the package task from this variant (or variant task group) or set packager_script if this variant is intended to run the packager."
    exit 1
fi

pushd "src/buildscripts" >&/dev/null
trap 'popd >& /dev/null' EXIT

$python \
    ${packager_script} \
    --prefix $(pwd)/.. \
    --distros \
    ${packager_distro} \
    --crypt_spec \
    --tarball $(pwd)/../mongo_crypt_shared_v1-${version}.${ext:-tgz} \
    -s ${version} \
    -m HEAD \
    -a ${packager_arch}
