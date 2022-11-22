DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose
set -o errexit

activate_venv
$python buildscripts/resmoke_tests_runtime_validate.py \
  --resmoke-report-file ./report.json \
  --project-id ${project_id} \
  --build-variant ${build_variant} \
  --task-name ${task_name}
