#!/usr/bin/env bash

set -o errexit
set -o nounset
set -o pipefail

repo_root="${BUILD_WORKSPACE_DIRECTORY:-$(pwd)}"
bazel_binary="${BAZEL_BINARY:-bazel}"
sentinel="$(mktemp /tmp/mongo-container-boundary.XXXXXXXX)"
trap 'rm -f -- "$sentinel"' EXIT

cd "$repo_root"

bazel_args=("$@")
environment_args=(
    "--action_env=MONGO_CONTAINER_BOUNDARY_SENTINEL=$sentinel"
    "--test_env=MONGO_CONTAINER_BOUNDARY_SENTINEL=$sentinel"
)
fixture="//bazel/toolchains/cc/mongo_linux/testdata/container_boundary"

"$bazel_binary" build "${bazel_args[@]}" "${environment_args[@]}" "$fixture:build_fixture"
"$bazel_binary" test "${bazel_args[@]}" "${environment_args[@]}" "$fixture:boundary_rust_test"
