set -o errexit verbose

ROOT_DIR="$(dirname $(realpath ${BASH_SOURCE[0]}))"
readonly ROOT_DIR
source "${ROOT_DIR}/prelude.sh"

activate_venv
[[ "${has_packages}" != "true" ]] && exit 0

if [[ -z "${packager_script+x}" ]]; then
  echo "Error: packager run when packager_script is not set, please remove the package task from this variant (or variant task group) or set packager_script if this variant is intended to run the packager."
  exit 1
fi

pushd "${ROOT_DIR}/src/buildscripts" >&/dev/null
trap 'popd >& /dev/null' EXIT

$python \
  ${packager_script} \
  --prefix $(pwd)/.. \
  --distros \
  --crypt_spec \
  ${packager_distro} \
  --tarball $(pwd)/../bazel-bin/mongo_crypt-stripped.tgz \
  -s ${version} \
  -m HEAD \
  -a ${packager_arch}
