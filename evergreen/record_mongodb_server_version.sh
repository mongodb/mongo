DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

build_patch_id="${build_patch_id:-${reuse_compile_from}}"
if [ -n "${build_patch_id}" ]; then
    exit 0
fi

binary="$1"
output_file="$2"
# MongoInstallRule's declared output remains available below bazel-bin even when the host-side
# convenience symlink cannot be recreated after a persistent container action. Prefer that
# declared output for this metadata-only probe instead of failing the entire debug archive task.
if [[ ! -e "$binary" ]]; then
    for fallback_binary in \
        "./bazel-bin/install-dist-test/bin/mongod" \
        "./bazel-bin/install-dist-test-debug/bin/mongod" \
        ./bazel-out/*/bin/install-dist-test/bin/mongod \
        ./bazel-out/*/bin/install-dist-test-debug/bin/mongod; do
        if [[ -e "$fallback_binary" ]]; then
            echo "INFO: $binary is unavailable; recording version from $fallback_binary" >&2
            binary="$fallback_binary"
            break
        fi
    done
fi
stderr_file="$(mktemp "${TMPDIR:-/tmp}/mongodb-version-stderr.XXXXXX")"
trap 'rm -f "$stderr_file"' EXIT

status=0
"$binary" --version >"$output_file" 2>"$stderr_file" || status=$?

if [[ "$status" -eq 0 ]]; then
    cat "$stderr_file" >&2
    exit 0
fi

echo "ERROR: MongoDB server version command failed with exit code $status: $binary --version" >&2
echo "Captured stdout:" >&2
cat "$output_file" >&2
echo "Captured stderr:" >&2
cat "$stderr_file" >&2

echo "Binary details for $binary:" >&2
if command -v file >/dev/null 2>&1; then
    file "$binary" >&2 || true
fi
if command -v ldd >/dev/null 2>&1; then
    ldd "$binary" >&2 || true
elif command -v otool >/dev/null 2>&1; then
    otool -L "$binary" >&2 || true
fi

exit "$status"
