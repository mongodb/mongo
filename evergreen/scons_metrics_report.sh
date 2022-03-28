DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose
set -o errexit

activate_venv
$python buildscripts/scons_metrics/report.py \
  --scons-stdout-log-file scons_stdout.log \
  --scons-cache-debug-log-file scons_cache.log \
  --cedar-report-file scons_cedar_report.json
