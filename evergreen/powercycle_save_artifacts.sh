DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose

if [ ! -f powercycle_ip_address.yml ]; then
  exit 0
fi

activate_venv
$python buildscripts/resmoke.py powercycle save-diagnostics
