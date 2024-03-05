function setup_mongo_task_generator {
  if [ ! -f mongo-task-generator ]; then
    curl -L https://github.com/mongodb/mongo-task-generator/releases/download/v0.7.12/mongo-task-generator --output mongo-task-generator
    chmod +x mongo-task-generator
  fi
}

## Comment above and uncomment below to test unreleased mongo-task-generator changes that are
## pushed to the `<branch-name>` of the `git@github.com:<user-name>/mongo-task-generator.git`
## repo
#function setup_mongo_task_generator {
#  if [ ! -f mongo-task-generator ]; then
#
#    curl https://sh.rustup.rs -sSf | sh -s -- -y
#    source "$HOME/.cargo/env"
#    git clone git@github.com:<user-name>/mongo-task-generator.git unreleased-mongo-task-generator
#    pushd unreleased-mongo-task-generator
#    git checkout <branch-name>
#    cargo build --release --locked
#    generator_path="$(pwd)/target/release/mongo-task-generator"
#    popd
#    cp "$generator_path" mongo-task-generator
#  fi
#}
