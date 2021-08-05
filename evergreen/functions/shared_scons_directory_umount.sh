DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose

if [ "${disable_shared_scons_cache}" = true ]; then
  exit
fi
if [ "${scons_cache_scope}" = "shared" ]; then
  if [ "Windows_NT" = "$OS" ]; then
    net use X: /delete || true
  else
    set_sudo
    $sudo umount /efs || true
  fi
fi
