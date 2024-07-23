DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

cd src

if [ -f monitor_build_status_command_exit_code.txt ]; then
  exit_code=$(cat monitor_build_status_command_exit_code.txt)
else
  exit_code=0
fi

echo "Exiting monitor_build_status with code $exit_code"
exit "$exit_code"
