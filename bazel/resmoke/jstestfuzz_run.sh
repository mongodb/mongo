#!/usr/bin/env bash
# Wrapper for the jstestfuzz_generate Bazel rule. Runs jstestfuzz's npm script
# (via a hermetic, rule-provided node) against the jstestfuzz checkout and
# collects its .js output into the rule's declared output directory.
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
bundle=""
npm_cli=""
js_tests_dir=""
npm_command=""
num_generated_files=""
branch=""
use_es_modules=0
seed=""
seed_offset=0
volatile_status=""

while [[ $# -gt 0 ]]; do
    case "$1" in
    --out-dir)
        out_dir=$2
        shift 2
        ;;
    --bundle)
        bundle=$2
        shift 2
        ;;
    --npm-cli)
        npm_cli=$2
        shift 2
        ;;
    --js-tests-dir)
        js_tests_dir=$2
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
    --seed-offset)
        seed_offset=$2
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

for required in out_dir bundle npm_cli npm_command num_generated_files; do
    if [[ -z "${!required}" ]]; then
        echo "jstestfuzz_run.sh: --${required//_/-} is required" >&2
        exit 2
    fi
done

# Make paths absolute before any cd so they stay valid.
exec_root=$PWD
out_dir_abs=$exec_root/$out_dir
npm_cli_abs=$exec_root/$npm_cli
[[ -n "$js_tests_dir" ]] && js_tests_dir=$exec_root/$js_tests_dir

node_bin_abs=${npm_cli_abs%/lib/node_modules/npm/bin/npm-cli.js}/bin
export PATH=$node_bin_abs:$PATH

# Unpack the prepared jstestfuzz checkout into a writable per-action dir. Using a
# tarball preserves node_modules' .bin/* symlinks.
jstestfuzz_run_dir=$scratch/jstestfuzz
mkdir -p "$jstestfuzz_run_dir"
npm_root=${npm_cli_abs%/bin/npm-cli.js}
node -e 'require(process.argv[1]).x({file: process.argv[2], cwd: process.argv[3], sync: true})' \
    "$npm_root/node_modules/tar" "$exec_root/$bundle" "$jstestfuzz_run_dir"

export TMP=${TMP:-${TMPDIR:-/tmp}}
export TMPDIR=$TMP
export HOME=$scratch
export npm_config_cache=$scratch/npm_cache
export npm_config_offline=true
export npm_config_update_notifier=false
export npm_config_fund=false

# git_repository strips .git/ after fetch, but jstestfuzz's file_namer.ts calls
# `git rev-parse --short=4 HEAD` to derive output-filename prefixes. The
# preserved SHA was captured by patch_cmds in MODULE.bazel; shim git so that
# one call returns the right answer. All other git invocations pass through.
sha_file=$jstestfuzz_run_dir/.jstestfuzz_commit_sha
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
# Sharded generation: each shard derives its own seed from the base seed so a
# fixed base seed stays reproducible per shard, and the seed embedded in every
# generated filename keeps shard outputs collision-free.
if ! [[ "$seed" =~ ^-?[0-9]+$ ]]; then
    echo "jstestfuzz_run.sh: seed must be an integer, got '$seed'" >&2
    exit 2
fi
if ! [[ "$seed_offset" =~ ^-?[0-9]+$ ]]; then
    echo "jstestfuzz_run.sh: seed-offset must be an integer, got '$seed_offset'" >&2
    exit 2
fi
seed=$((seed + seed_offset))

mkdir -p "$out_dir_abs"

cmd=(node "$npm_cli_abs" run "$npm_command" --
    --numGeneratedFiles "$num_generated_files"
    --branch "$branch"
    --seed "$seed"
    --out "$out_dir_abs")
if [[ -n "$js_tests_dir" ]]; then
    cmd+=(--jsTestsDir "$js_tests_dir")
fi
if [[ $use_es_modules -eq 1 ]]; then
    cmd+=(--useEsModules)
fi
cmd+=("${extra_args[@]}")

(
    cd "$jstestfuzz_run_dir"
    "${cmd[@]}"
) >>"$log" 2>&1

shopt -s nullglob
generated=("$out_dir_abs"/*.js)
if [[ ${#generated[@]} -eq 0 ]]; then
    echo "jstestfuzz_run.sh: jstestfuzz produced no .js files in $out_dir_abs" >&2
    exit 1
fi

# Record the seed so it can be inspected and used to reproduce tests
echo "$seed" >"$out_dir_abs/.jstestfuzz_seed"

# Record the upstream jstestfuzz commit alongside the seed. Both are needed
# to reproduce: same seed + different jstestfuzz code = different tests.
# fetch_remote_test_results.sh harvests this and pins it via --repo_env=JSTESTFUZZ_COMMIT=.
if [[ -f "$jstestfuzz_run_dir/.jstestfuzz_commit_sha" ]]; then
    cp "$jstestfuzz_run_dir/.jstestfuzz_commit_sha" "$out_dir_abs/.jstestfuzz_commit_sha"
fi
