DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit

activate_venv

# Set the suite name to be the task name by default; unless overridden with the `suite` expansion.
suite_name=${task_name}
if [[ -n ${suite} ]]; then
  suite_name=${suite}
fi

$python buildscripts/benchmarks/analyze.py \
  --evg-api-config ./.evergreen.yml \
  --task-id ${task_id} \
  --suite ${suite}
