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

$python buildscripts/resmoke.py setup-multiversion \
  --installDir /data/install \
  --linkDir /data/multiversion \
  --edition $edition \
  --platform $platform \
  --architecture $architecture \
  --useLatest 4.0

# The platform and architecture for how some of the binaries are reported in
# https://downloads.mongodb.org/full.json changed between MongoDB 4.0 and MongoDB 4.2.
# Certain build variants define additional multiversion_*_42_or_later expansions in order to
# be able to fetch a complete set of versions.

if [ ! -z "${multiversion_edition_42_or_later}" ]; then
  edition="${multiversion_edition_42_or_later}"
fi

if [ ! -z "${multiversion_platform_42_or_later}" ]; then
  platform="${multiversion_platform_42_or_later}"
fi

if [ ! -z "${multiversion_architecture_42_or_later}" ]; then
  architecture="${multiversion_architecture_42_or_later}"
fi

$python buildscripts/resmoke.py setup-multiversion \
  --installDir /data/install \
  --linkDir /data/multiversion \
  --edition $edition \
  --platform $platform \
  --architecture $architecture \
  --useLatest 4.2

# The platform and architecture for how some of the binaries are reported in
# https://downloads.mongodb.org/full.json changed between MongoDB 4.2 and MongoDB 4.4.
# Certain build variants define additional multiversion_*_44_or_later expansions in order to
# be able to fetch a complete set of versions.

if [ ! -z "${multiversion_edition_44_or_later}" ]; then
  edition="${multiversion_edition_44_or_later}"
fi

if [ ! -z "${multiversion_platform_44_or_later}" ]; then
  platform="${multiversion_platform_44_or_later}"
fi

if [ ! -z "${multiversion_architecture_44_or_later}" ]; then
  architecture="${multiversion_architecture_44_or_later}"
fi

last_lts_arg="--installLastLTS"
last_continuous_arg="--installLastContinuous"

if [[ -n "${last_lts_evg_version_id}" ]]; then
  last_lts_arg="${last_lts_evg_version_id}"
fi

if [[ -n "${last_continuous_evg_version_id}" ]]; then
  last_continuous_arg="${last_continuous_evg_version_id}"
fi

$python buildscripts/resmoke.py setup-multiversion \
  --installDir /data/install \
  --linkDir /data/multiversion \
  --edition $edition \
  --platform $platform \
  --architecture $architecture \
  --useLatest \
  $last_lts_arg \
  $last_continuous_arg 4.4
