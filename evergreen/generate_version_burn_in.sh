DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

setup_mongo_task_generator
activate_venv

base_revision="$(git merge-base ${revision} HEAD)"
echo "Base patch revision: $base_revision"

$python buildscripts/burn_in_tests.py generate-test-membership-map-file-for-ci

RUST_BACKTRACE=full PATH=$PATH:$HOME:/ ./mongo-task-generator \
  --expansion-file ../expansions.yml \
  --evg-auth-file ./.evergreen.yml \
  --evg-project-file ${evergreen_config_file_path} \
  --generate-sub-tasks-config etc/generate_subtasks_config.yml \
  --s3-test-stats-endpoint https://mongo-test-stats.s3.amazonaws.com \
  --burn-in \
  --burn-in-tests-command "python buildscripts/burn_in_tests.py run --origin-rev=$base_revision" \
  $@
