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
releases_file=$9

log=$(mktemp)
trap 'rm -f "$log"' EXIT

# Point git at the real repository so tag lookups (needed for patch versions
# like 8.0.16 and for the last-patch alias) work when db-contrib-tool runs from
# a Bazel output directory that has no .git ancestor. GIT_WORK_TREE is required
# alongside GIT_DIR: the plain git CLI resolves GIT_DIR alone fine, but
# GitPython -- used by resmoke's last-patch tag resolution -- raises
# InvalidGitRepositoryError without GIT_WORK_TREE, since it does not honor a
# GIT_DIR pointing at a linked worktree's .git file the same way.
mongo_root="$(dirname "$(readlink -f "$root/MODULE.bazel")")"
export GIT_DIR="$mongo_root/.git"
export GIT_WORK_TREE="$mongo_root"

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
        --resmokeCmd "$root/$resmoke --mongoVersionFile=$root/$version_file --releasesFile=$root/$releases_file"
) >"$log" 2>&1; then
    cat "$log" >&2
    exit 1
fi
