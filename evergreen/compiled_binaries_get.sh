DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# activate_venv will make sure we are using python 3
activate_venv
setup_db_contrib_tool

rm -rf /data/install /data/multiversion

edition="${multiversion_edition}"
platform="${multiversion_platform}"
architecture="${multiversion_architecture}"

version=${project#mongodb-mongo-}
version=${version#v}

if [ ! -z "${multiversion_platform_50_or_later}" ]; then
  platform="${multiversion_platform_50_or_later}"
fi

# This is primarily for tests for infrastructure which don't always need the latest
# binaries.
db-contrib-tool setup-repro-env \
  --installDir /data/install \
  --linkDir /data/multiversion \
  --edition $edition \
  --platform $platform \
  --architecture $architecture \
  --evgVersionsFile multiversion-downloads.json \
  $version

dist_test_dir=$(find /data/install -type d -iname "dist-test")
mv "$dist_test_dir" "$(pwd)"
