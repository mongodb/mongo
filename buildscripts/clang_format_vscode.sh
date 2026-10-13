#!/usr/bin/env bash
set -euo pipefail

# Resolve the pinned repository through Bazel instead of depending on convenience
# symlinks or a canonical repository name, which can change between Bazel versions.
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# The editor extension rejects any stderr, even when the formatter succeeds.
# Keep Bazel's setup diagnostics for failures without hiding formatter diagnostics.
BAZEL_STDERR="$(mktemp)"
trap 'rm -f -- "$BAZEL_STDERR"' EXIT
# Query fetches the repository if needed. Keep the editor's stdin for clang-format.
# Keep comments outside command substitutions: Bash 3.2 misparses apostrophes in them.
if LOCATION="$(
    cd -- "$REPO_ROOT" || exit
    bazel query --noshow_progress --bes_backend= --bes_results_url= \
        --output=location @mongo_toolchain_v5//:clang_format </dev/null 2>"$BAZEL_STDERR"
)"; then
    rm -f -- "$BAZEL_STDERR"
    trap - EXIT
else
    QUERY_STATUS=$?
    cat -- "$BAZEL_STDERR" >&2
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

# Preserve arguments, stdin, stdout, working directory, and the formatter's status.
exec "$CLANG_FORMAT" "$@"
