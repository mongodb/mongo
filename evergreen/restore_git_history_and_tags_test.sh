#!/usr/bin/env bash

set -euo pipefail

source_helper="${TEST_SRCDIR}/${TEST_WORKSPACE}/evergreen/restore_git_history_and_tags.sh"
test_root="${TEST_TMPDIR}/restore_git_history"
mkdir -p "${test_root}/bin"

# Only the platform detection is mocked; cloning, patching, and fetching use Git.
cat >"${test_root}/bin/uname" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$TEST_GIT_PLATFORM"
EOF
chmod +x "${test_root}/bin/uname"
# Provide command lookup in minimal Git containers that omit which.
which() { command -v "$@"; }
export -f which
export PATH="${test_root}/bin:${PATH}"
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL=/dev/null

origin="${test_root}/origin"
git init --quiet --initial-branch=master "$origin"
git -C "$origin" config uploadpack.allowFilter true
git -C "$origin" config user.name 'Git history test'
git -C "$origin" config user.email git-history-test@example.invalid
for revision in 1 2 3; do
    printf '%s\n' "$revision" >"${origin}/file"
    git -C "$origin" add file
    git -C "$origin" commit --quiet -m "revision $revision"
done
historical_commit=$(git -C "$origin" rev-parse HEAD~2)
historical_tree=$(git -C "$origin" rev-parse "${historical_commit}^{tree}")
git -C "$origin" tag -a v1 -m 'historical release' "$historical_commit"
git -C "$origin" tag -a v3 -m 'current release'

for platform in MINGW64_NT-10.0 CYGWIN_NT-10.0 MSYS_NT-10.0 Linux; do
    export TEST_GIT_PLATFORM="$platform"
    checkout="${test_root}/${platform}"
    git clone --quiet --depth=1 --no-tags "file://${origin}" "$checkout"
    [[ -f "${checkout}/.git/shallow" ]]
    printf '%s\n' 'staged patch edit' >"${checkout}/file"
    git -C "$checkout" add file
    before_patch=$(git -C "$checkout" diff --cached)

    if ! dir="$checkout" bash "$source_helper" >"${checkout}/restore.log" 2>&1; then
        cat "${checkout}/restore.log" >&2
        exit 1
    fi

    [[ ! -f "${checkout}/.git/shallow" ]]
    [[ "$(git -C "$checkout" rev-list --count HEAD)" -eq 3 ]]
    [[ "$(git -C "$checkout" rev-parse 'v1^{commit}')" == "$historical_commit" ]]
    [[ "$(git -C "$checkout" describe)" == v3 ]]
    [[ "$(git -C "$checkout" diff --cached)" == "$before_patch" ]]

    if [[ "$platform" != Linux ]]; then
        # A treeless unshallow fetch leaves this historical tree missing. New Git
        # then crashes when repacking links to such objects. Require the tree to
        # be available even when lazy fetching cannot contact the origin.
        git -C "$checkout" config remote.origin.url "${test_root}/missing-origin"
        git -C "$checkout" cat-file -p "$historical_tree" >/dev/null
        # Re-running setup on an already restored checkout needs no network.
        if ! dir="$checkout" bash "$source_helper" >"${checkout}/restore.log" 2>&1; then
            cat "${checkout}/restore.log" >&2
            exit 1
        fi
        [[ "$(git -C "$checkout" diff --cached)" == "$before_patch" ]]
    fi
    echo "History, tags, and staged edits preserved for $platform"
done
