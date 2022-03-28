DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jstestfuzz

set -o errexit
set -o verbose

add_nodejs_to_path

in_patch_build_flag=""
if [[ "${is_patch}" = "true" ]]; then
  case "${npm_command}" in
  agg-fuzzer | query-fuzzer)
    in_patch_build_flag="--inPatchBuild"
    ;;
  esac
fi

eval npm run "${npm_command}" -- "${jstestfuzz_vars}" "${in_patch_build_flag}" --branch "${branch_name}"
