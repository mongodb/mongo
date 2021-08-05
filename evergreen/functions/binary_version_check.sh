DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
mongo_binary=dist-test/bin/mongo${exe}
activate_venv
bin_ver=$($python -c "import yaml; print(yaml.safe_load(open('compile_expansions.yml'))['version']);" | tr -d '[ \r\n]')
# Due to SERVER-23810, we cannot use $mongo_binary --quiet --nodb --eval "version();"
mongo_ver=$($mongo_binary --version | perl -pe '/version v([^\"]*)/; $_ = $1;' | tr -d '[ \r\n]')
# The versions must match
if [ "$bin_ver" != "$mongo_ver" ]; then
  echo "The mongo version is $mongo_ver, expected version is $bin_ver"
  exit 1
fi
