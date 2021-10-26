DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/buildscripts/package_test

set -o errexit

export KITCHEN_ARTIFACTS_URL="https://s3.amazonaws.com/mciuploads/${project}/${build_variant}/${revision}/artifacts/${build_id}-packages.tgz"
export KITCHEN_SECURITY_GROUP="${kitchen_security_group}"
export KITCHEN_SSH_KEY_ID="${kitchen_ssh_key_id}"
export KITCHEN_SUBNET="${kitchen_subnet}"
export KITCHEN_VPC="${kitchen_vpc}"

if [ ${packager_distro} == "suse12" ]; then
  export KITCHEN_YAML="kitchen.legacy.yml"
fi

if [[ "${packager_arch}" == "aarch64" || "${packager_arch}" == "arm64" ]]; then
  kitchen_packager_distro="${packager_distro}-arm64"
else
  kitchen_packager_distro="${packager_distro}-x86-64"
fi

activate_venv
# set expiration tag 2 hours in the future, since no test should take this long
export KITCHEN_EXPIRE="$($python -c 'import datetime; print((datetime.datetime.utcnow() + datetime.timedelta(hours=2)).strftime("%Y-%m-%d %H:%M:%S"))')"

for i in {1..3}; do
  if ! kitchen verify $kitchen_packager_distro; then
    verified="false"
    kitchen destroy $kitchen_packager_distro || true
    sleep 30
  else
    verified="true"
    break
  fi
done

kitchen destroy $kitchen_packager_distro || true
test "$verified" = "true"
