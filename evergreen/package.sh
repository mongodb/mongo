DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

if [ $(find . -name mongocryptd${exe} | wc -l) -eq 1 ]; then
  # Validate that this build_variant is listed as a known enterprise task for mongocryptd
  eval PATH=$PATH:$HOME $python ../buildscripts/validate_mongocryptd.py --variant "${build_variant}" ../etc/evergreen.yml
fi
