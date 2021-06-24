DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
if [ "${has_packages}" = "true" ]; then
  cd buildscripts
  $python ${packager_script} --prefix $(pwd)/.. --distros ${packager_distro} --tarball $(pwd)/../mongodb-dist.tgz -s ${version} -m HEAD -a ${packager_arch}
  cd ..
fi
