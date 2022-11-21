DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

setup_mongo_task_generator
activate_venv
PATH=$PATH:$HOME:/ ./mongo-task-generator \
  --expansion-file ../expansions.yml \
  --evg-auth-file ./.evergreen.yml \
  --evg-project-file ${evergreen_config_file_path} \
  --generate-sub-tasks-config etc/generate_subtasks_config.yml \
  --s3-test-stats-endpoint https://mongo-test-stats.s3.amazonaws.com \
  $@
