DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set -o errexit
set -o verbose

# LTO compiles create a bind mount to circumvent disk space limitations. This is a cleanup step.
set_sudo
$sudo umount /tmp || true
