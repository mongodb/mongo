function setup_mongo_task_generator {
    if [ ! -f mongo-task-generator ]; then
        local version="v3.11.0"
        local sha256="2efa8a27f2628f65936fa65dad2a131abcecd28776f9ce5a4f960a2726b9f53b"
        curl -fL --retry 5 --retry-delay 10 --retry-max-time 120 "https://github.com/mongodb/mongo-task-generator/releases/download/${version}/mongo-task-generator" --output mongo-task-generator
        if ! echo "${sha256}  mongo-task-generator" | { sha256sum -c - || shasum -a 256 -c -; } >/dev/null 2>&1; then
            echo "mongo-task-generator ${version} checksum mismatch" >&2
            rm -f mongo-task-generator
            return 1
        fi
        chmod +x mongo-task-generator
    fi
}

## Comment above and uncomment below to test unreleased mongo-task-generator changes that are
## pushed to the `<branch-name>` of the `https://github.com/mongodb/mongo-task-generator.git`
## repo
#function setup_mongo_task_generator {
#  if [ ! -f mongo-task-generator ]; then
#
#    curl https://sh.rustup.rs -sSf | sh -s -- -y
#    source "$HOME/.cargo/env"
#    git clone https://github.com/mongodb/mongo-task-generator.git unreleased-mongo-task-generator
#    pushd unreleased-mongo-task-generator
#    git checkout <branch-name>
#    cargo build --release --locked
#    generator_path="$(pwd)/target/release/mongo-task-generator"
#    popd
#    cp "$generator_path" mongo-task-generator
#  fi
#}
