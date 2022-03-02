DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit

# Set the suite name to be the task name by default; unless overridden with the `suite` expansion.
suite_name=${task_name}
if [[ -n ${suite} ]]; then
  suite_name=${suite}
fi

timeout_factor=""
if [[ -n "${exec_timeout_factor}" ]]; then
  timeout_factor="--exec-timeout-factor ${exec_timeout_factor}"
fi

activate_venv
PATH=$PATH:$HOME:/ $python buildscripts/evergreen_task_timeout.py $timeout_factor \
  --task-name ${task_name} \
  --suite-name ${suite_name} \
  --build-variant ${build_variant} \
  --evg-alias '${alias}' \
  --timeout ${timeout_secs} \
  --exec-timeout ${exec_timeout_secs} \
  --evg-api-config ./.evergreen.yml \
  --out-file task_timeout_expansions.yml
