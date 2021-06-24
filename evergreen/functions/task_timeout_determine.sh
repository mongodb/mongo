DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit
activate_venv
$python buildscripts/evergreen_task_timeout.py \
  --task-name ${task_name} \
  --build-variant ${build_variant} \
  --evg-alias '${alias}' \
  --timeout ${timeout_secs} \
  --exec-timeout ${exec_timeout_secs} \
  --out-file task_timeout_expansions.yml
