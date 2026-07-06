#!/usr/bin/env bash
# Wrapper for the jstestfuzz_generate Bazel rule. Invokes the upstream
# jstestfuzz npm script in a local checkout and collects its .js output into
# the rule's declared output directory.
#
# All flags are passed by the rule; positional/extra jstestfuzz args follow
# the literal '--' separator.

set -euo pipefail

# Per-run scratch dir used for the temp log (and the optional git shim below).
# Cleaned up on exit; the log is dumped to stderr if any step fails.
scratch=$(mktemp -d)
log=$scratch/jstestfuzz.log
: >"$log"
cleanup() {
    local rc=$?
    if [[ $rc -ne 0 && -s "$log" ]]; then
        cat "$log" >&2
    fi
    rm -rf "$scratch"
    return $rc
}
trap cleanup EXIT

out_dir=""
jstestfuzz_root=""
npm_command=""
num_generated_files=""
branch=""
use_es_modules=0
seed=""
volatile_status=""

while [[ $# -gt 0 ]]; do
    case "$1" in
    --out-dir)
        out_dir=$2
        shift 2
        ;;
    --jstestfuzz-root)
        jstestfuzz_root=$2
        shift 2
        ;;
    --npm-command)
        npm_command=$2
        shift 2
        ;;
    --num-generated-files)
        num_generated_files=$2
        shift 2
        ;;
    --branch)
        branch=$2
        shift 2
        ;;
    --use-es-modules)
        use_es_modules=1
        shift
        ;;
    --seed)
        seed=$2
        shift 2
        ;;
    --volatile-status)
        volatile_status=$2
        shift 2
        ;;
    --)
        shift
        break
        ;;
    *)
        echo "jstestfuzz_run.sh: unknown flag: $1" >&2
        exit 2
        ;;
    esac
done
extra_args=("$@")

for required in out_dir jstestfuzz_root npm_command num_generated_files; do
    if [[ -z "${!required}" ]]; then
        echo "jstestfuzz_run.sh: --${required//_/-} is required" >&2
        exit 2
    fi
done

# Make paths absolute before any cd so they stay valid.
exec_root=$PWD
out_dir_abs=$exec_root/$out_dir
jstestfuzz_root_abs=$exec_root/$jstestfuzz_root

# When the repository rule used a pre-existing checkout (Evergreen module or
# local clone), individual files in jstestfuzz_root_abs are symlinks to the
# real checkout.  TypeScript (ts-node) resolves tsconfig extends paths relative
# to the real file location, not the symlink, so node_modules must be installed
# there too.  Detect this case and run npm from the real checkout directory,
# mirroring the old evergreen jstestfuzz_run.sh which always cd'd to src/jstestfuzz.
jstestfuzz_run_dir=$jstestfuzz_root_abs
if [[ -L "$jstestfuzz_root_abs/package.json" ]]; then
    jstestfuzz_run_dir=$(dirname "$(realpath "$jstestfuzz_root_abs/package.json")")
fi

# Upstream npm_run.sh derives npm_config_cache from $TMP; if unset it falls back
# to /npm_cache (root-owned). Point it at a writable per-action location.
export TMP=${TMP:-${TMPDIR:-/tmp}}
export TMPDIR=$TMP

# git_repository strips .git/ after fetch, but jstestfuzz's file_namer.ts calls
# `git rev-parse --short=4 HEAD` to derive output-filename prefixes. The
# preserved SHA was captured by patch_cmds in MODULE.bazel; shim git so that
# one call returns the right answer. All other git invocations pass through.
sha_file=$jstestfuzz_root_abs/.jstestfuzz_commit_sha
if [[ -f "$sha_file" ]]; then
    sha=$(tr -d '[:space:]' <"$sha_file")
    real_git=$(command -v git || echo /usr/bin/git)
    shim_dir=$scratch/git_shim
    mkdir -p "$shim_dir"
    cat >"$shim_dir/git" <<EOF
#!/usr/bin/env bash
if [[ "\$1" == "rev-parse" && "\$2" == "--short=4" && "\$3" == "HEAD" ]]; then
    echo "${sha:0:4}"
    exit 0
fi
exec "$real_git" "\$@"
EOF
    chmod +x "$shim_dir/git"
    export PATH=$shim_dir:$PATH
fi

# Derive a seed from the build timestamp when none was pinned.
if [[ -z "$seed" ]]; then
    if [[ -z "$volatile_status" ]]; then
        echo "jstestfuzz_run.sh: --seed or --volatile-status required" >&2
        exit 2
    fi
    seed=$(awk '/^BUILD_TIMESTAMP / {print $2}' "$volatile_status" | tr -dc '0-9' | head -c 9)
    # Fallback to $RANDOM
    if [[ -z "$seed" ]]; then
        seed=$RANDOM
    fi
fi

mkdir -p "$out_dir_abs"

# Run jstestfuzz inside its checkout. Outputs land in jstestfuzz/out/ (the
# upstream default); we move them into the bazel-declared output dir at the end.
upstream_out=$jstestfuzz_run_dir/out
rm -rf "$upstream_out"
mkdir -p "$upstream_out"

cmd=(./src/scripts/npm_run.sh "$npm_command" --
    --numGeneratedFiles "$num_generated_files"
    --branch "$branch"
    --seed "$seed")
if [[ $use_es_modules -eq 1 ]]; then
    cmd+=(--useEsModules)
fi

# Default --jsTestsDir to the workspace's jstests/ (resolvable from the exec
# root because we run unsandboxed). Authors can override via extra_args.
have_js_tests_dir=0
for a in "${extra_args[@]:-}"; do
    if [[ "$a" == "--jsTestsDir" || "$a" == --jsTestsDir=* ]]; then
        have_js_tests_dir=1
        break
    fi
done
if [[ $have_js_tests_dir -eq 0 ]]; then
    cmd+=(--jsTestsDir "$exec_root/jstests")
fi
cmd+=("${extra_args[@]}")

(
    cd "$jstestfuzz_run_dir"
    "${cmd[@]}"
) >>"$log" 2>&1

shopt -s nullglob
generated=("$upstream_out"/*.js)
if [[ ${#generated[@]} -eq 0 ]]; then
    echo "jstestfuzz_run.sh: jstestfuzz produced no .js files in $upstream_out" >&2
    exit 1
fi
mv "${generated[@]}" "$out_dir_abs/"

# Record the seed so it can be inspected and used to reproduce tests
echo "$seed" >"$out_dir_abs/.jstestfuzz_seed"

# Record the upstream jstestfuzz commit alongside the seed. Both are needed
# to reproduce: same seed + different jstestfuzz code = different tests.
# fetch_remote_test_results.sh harvests this and pins it via --repo_env=JSTESTFUZZ_COMMIT=.
if [[ -f "$jstestfuzz_root_abs/.jstestfuzz_commit_sha" ]]; then
    cp "$jstestfuzz_root_abs/.jstestfuzz_commit_sha" "$out_dir_abs/.jstestfuzz_commit_sha"
fi
