DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

curl -L https://github.com/mongodb/mongo-task-generator/releases/download/v0.5.3/mongo-task-generator --output mongo-task-generator
chmod +x mongo-task-generator

## Comment above and uncomment below to test unreleased mongo-task-generator changes pushed to <branch-name>
#curl https://sh.rustup.rs -sSf | sh -s -- -y
#source "$HOME/.cargo/env"
#git clone git@github.com:mongodb/mongo-task-generator.git unreleased-mongo-task-generator
#pushd unreleased-mongo-task-generator
#git checkout <branch-name>
#cargo build --release --locked
#generator_path="$(pwd)/target/release/mongo-task-generator"
#popd
#cp "$generator_path" mongo-task-generator

activate_venv
PATH=$PATH:$HOME:/ ./mongo-task-generator \
  --expansion-file ../expansions.yml \
  --evg-auth-file ./.evergreen.yml \
  --evg-project-file ${evergreen_config_file_path} \
  --generate-sub-tasks-config etc/generate_subtasks_config.yml \
  --burn-in \
  $@
