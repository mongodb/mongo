DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

if [ -n "$GENERATE_BUILD_VARIANTS" ]; then
  echo "Skipping generation since 'generate_build_variants' is set."
  exit 0
fi

cd src

set -o errexit

activate_venv
$python buildscripts/evergreen_generate_resmoke_tasks.py --expansion-file ../expansions.yml --verbose
