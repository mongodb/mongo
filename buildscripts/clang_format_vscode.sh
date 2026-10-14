#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_VERSION="$(cat -- "$REPO_ROOT/buildscripts/clang_format_version.txt")"
if [[ -z "$EXPECTED_VERSION" ]]; then
    printf 'The clang-format version pin is empty. Run bazel run lint --fix.\n' >&2
    exit 1
fi
CACHE_FILE="$REPO_ROOT/.cache/clang-format-vscode"

matches_version() {
    local version
    [[ -f "$1" && -x "$1" ]] || return 1
    version="$("$1" --version </dev/null 2>/dev/null)" || return 1
    [[ "$version" == "$EXPECTED_VERSION" ]]
}

remember_formatter() {
    local temporary
    # The cache is optional (for example, a checkout may be read-only). Publish
    # the path atomically so simultaneous saves cannot read a partial entry.
    mkdir -p -- "$REPO_ROOT/.cache" 2>/dev/null || return 0
    temporary="$(mktemp "$CACHE_FILE.XXXXXX" 2>/dev/null)" || return 0
    if ! { printf '%s\n' "$1" >"$temporary" && mv -f -- "$temporary" "$CACHE_FILE"; } 2>/dev/null; then
        rm -f -- "$temporary" 2>/dev/null || true
    fi
}

# Probe the binary itself: a branch switch changes the pin and Bazel may replace
# a toolchain at the same path. Neither check requires a Bazel server lock.
if [[ -f "$CACHE_FILE" ]] && IFS= read -r CLANG_FORMAT <"$CACHE_FILE" && matches_version "$CLANG_FORMAT"; then
    exec "$CLANG_FORMAT" "$@"
fi

# Discover repositories fetched by a build before the first editor invocation.
# The wildcard accommodates Bazel canonical repository naming changes.
for CLANG_FORMAT in "$REPO_ROOT"/bazel-*/external/*mongo_toolchain_v5*/v5/bin/clang-format; do
    if matches_version "$CLANG_FORMAT"; then
        remember_formatter "$CLANG_FORMAT"
        exec "$CLANG_FORMAT" "$@"
    fi
done

# Resolve the pinned repository through Bazel only when no matching local binary
# is available. A busy server must never block an editor save.
# The editor extension rejects any stderr, even when the formatter succeeds.
# Keep Bazel's setup diagnostics for failures without hiding formatter diagnostics.
BAZEL_STDERR="$(mktemp)"
trap 'rm -f -- "$BAZEL_STDERR"' EXIT
# Query fetches the repository if needed. Keep the editor's stdin for clang-format.
# Keep comments outside command substitutions: Bash 3.2 misparses apostrophes in them.
if LOCATION="$(
    cd -- "$REPO_ROOT" || exit
    BAZELISK_SKIP_WRAPPER=1 bazel --noblock_for_lock query --noshow_progress --bes_backend= --bes_results_url= \
        --output=location @mongo_toolchain_v5//:clang_format </dev/null 2>"$BAZEL_STDERR"
)"; then
    rm -f -- "$BAZEL_STDERR"
    trap - EXIT
else
    QUERY_STATUS=$?
    cat -- "$BAZEL_STDERR" >&2
    printf 'Could not prepare clang-format. Run bazel run lint when Bazel is available.\n' >&2
    exit "$QUERY_STATUS"
fi

# Match the trailing line and column rather than splitting on the first colon.
LOCATION_PATTERN='^(.+):[0-9]+:[0-9]+:'
if [[ ! "$LOCATION" =~ $LOCATION_PATTERN ]]; then
    printf 'Could not locate Bazel clang-format: %s\n' "$LOCATION" >&2
    exit 1
fi
BUILD_FILE="${BASH_REMATCH[1]}"
if [[ "$BUILD_FILE" != /* ]]; then
    BUILD_FILE="$REPO_ROOT/$BUILD_FILE"
fi
CLANG_FORMAT="${BUILD_FILE%/*}/v5/bin/clang-format"
if [[ ! -f "$CLANG_FORMAT" || ! -x "$CLANG_FORMAT" ]]; then
    printf 'Bazel clang-format is not executable: %s\n' "$CLANG_FORMAT" >&2
    exit 1
fi

if ! matches_version "$CLANG_FORMAT"; then
    printf 'Bazel clang-format does not match the version pin. Run bazel run lint --fix.\n' >&2
    exit 1
fi
remember_formatter "$CLANG_FORMAT"

# Preserve arguments, stdin, stdout, working directory, and the formatter's status.
exec "$CLANG_FORMAT" "$@"
