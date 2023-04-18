DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

podman login --username ${release_tools_container_registry_username} --password ${release_tools_container_registry_password} ${release_tools_container_registry}
