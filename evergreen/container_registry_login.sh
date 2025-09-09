DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -oe
attempts=0
max_attempts=4

while ! aws ecr get-login-password --region us-east-1 | podman login --password-stdin --username ${release_tools_container_registry_username_ecr} ${release_tools_container_registry_ecr}; do
  [ "$attempts" -ge "$max_attempts" ] && exit 1
  ((attempts++))
  sleep 10
done
