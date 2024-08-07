DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set +o errexit

cd src

if [ -z "${PGO_PROFILE_URL:-}" ]; then
  echo "No pgo profile url specified" >&2
  exit 0
fi

wget $PGO_PROFILE_URL
tar -xvf default.profdata.tgz
