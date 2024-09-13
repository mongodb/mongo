DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -oe
attempts=0
max_attempts=4

while ! echo "${release_tools_container_registry_password}" | podman login --password-stdin --username ${release_tools_container_registry_username} ${release_tools_container_registry}; do
  [ "$attempts" -ge "$max_attempts" ] && exit 1
  ((attempts++))
  sleep 10
done
