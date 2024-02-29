DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
if [ "${has_packages}" != "true" ]; then
  exit 0
  # TODO: SERVER-80443 add back validation
  # echo "Error: packager run when has_packages is not set to true, please remove the package task from this variant (or variant task group) or set has_packages to true if this variant is intended to run the packager."
  # exit 1
fi

if [ -z ${packager_script+x} ]; then
  echo "Error: packager run when packager_script is not set, please remove the package task from this variant (or variant task group) or set packager_script if this variant is intended to run the packager."
  exit 1
fi

cd buildscripts
$python ${packager_script} --prefix $(pwd)/.. --distros ${packager_distro} --tarball $(pwd)/../mongodb-dist.tgz -s ${version} -m HEAD -a ${packager_arch}
cd ..
