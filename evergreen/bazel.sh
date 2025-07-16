DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

source "$DIR/bazel_utility_functions.sh"
(
    cd $DIR/..
    exec $(bazel_get_binary_path) "$@"
)
