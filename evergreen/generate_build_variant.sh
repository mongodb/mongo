DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

activate_venv
PATH=$PATH:$HOME:/ $python buildscripts/evergreen_gen_build_variant.py \
  --expansion-file ../expansions.yml \
  --evg-api-config ./.evergreen.yml \
  --output-file ${build_variant}.json \
  --verbose
