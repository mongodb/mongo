DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

rm -rf /data/install /data/multiversion

edition="${multiversion_edition}"
platform="${multiversion_platform}"
architecture="${multiversion_architecture}"

if [ ! -z "${multiversion_edition_42_or_later}" ]; then
  edition="${multiversion_edition_42_or_later}"
fi
if [ ! -z "${multiversion_platform_42_or_later}" ]; then
  platform="${multiversion_platform_42_or_later}"
fi
if [ ! -z "${multiversion_architecture_42_or_later}" ]; then
  architecture="${multiversion_architecture_42_or_later}"
fi

if [ ! -z "${multiversion_edition_44_or_later}" ]; then
  edition="${multiversion_edition_44_or_later}"
fi
if [ ! -z "${multiversion_platform_44_or_later}" ]; then
  platform="${multiversion_platform_44_or_later}"
fi
if [ ! -z "${multiversion_architecture_44_or_later}" ]; then
  architecture="${multiversion_architecture_44_or_later}"
fi

version=${project#mongodb-mongo-}
version=${version#v}

# This is primarily for tests for infrastructure which don't always need the latest
# binaries.
$python buildscripts/resmoke.py setup-multiversion \
  --installDir /data/install \
  --linkDir /data/multiversion \
  --edition $edition \
  --platform $platform \
  --architecture $architecture \
  --useLatest $version

version_dir=$(find /data/install -type d -iname "*$version*")
mv $version_dir/dist-test $(pwd)
