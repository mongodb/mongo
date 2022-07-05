DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

curl -L https://github.com/mongodb/mongo-task-generator/releases/download/v0.4.5/mongo-task-generator --output mongo-task-generator
chmod +x mongo-task-generator

activate_venv
PATH=$PATH:$HOME:/ ./mongo-task-generator \
  --expansion-file ../expansions.yml \
  --evg-auth-file ./.evergreen.yml \
  --evg-project-file ${evergreen_config_file_path} \
  --generate-sub-tasks-config etc/generate_subtasks_config.yml \
  $@
