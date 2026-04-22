#!/usr/bin/env bash
# Wrapper for db-contrib-tool setup-repro-env invoked from a Bazel action.

set -euo pipefail

root=$PWD
tool=$1
version_arg=$2
edition=$3
install_dir=$4
link_dir=$5
evg_versions_file=$6
resmoke=$7
version_file=$8

log=$(mktemp)
trap 'rm -f "$log"' EXIT

# cd into the per-invocation output dir so that db-contrib-tool's temporary
# 'multiversion-config.yml' is isolated from parallel invocations.  The output
# dir is under the Bazel build root, so parent-directory search for
# .evergreen.yml still reaches $HOME.
if ! (
    cd "$root/$link_dir"
    "$root/$tool" setup-repro-env "$version_arg" \
        --edition "$edition" \
        --installDir "$root/$install_dir" \
        --linkDir "$root/$link_dir" \
        --evgVersionsFile "$root/$evg_versions_file" \
        --resmokeCmd "$root/$resmoke --mongoVersionFile=$root/$version_file"
) >"$log" 2>&1; then
    cat "$log" >&2
    exit 1
fi
