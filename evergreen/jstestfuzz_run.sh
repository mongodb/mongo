DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -evo pipefail

cd src/jstestfuzz

in_patch_build_flag=""
if [[ "${is_patch}" = "true" ]]; then
  case "${npm_command}" in
  agg-fuzzer | query-fuzzer)
    in_patch_build_flag="--inPatchBuild"
    ;;
  esac
fi

# TODO(DEVPROD-10137): Remove this conditional logic once `--useEsModule` is a top-level supported flag of jstestfuzz.
maybe_use_es_modules=""
if [[ "${npm_command}" != "jstestfuzz" ]]; then
  maybe_use_es_modules="--useEsModules"
fi

./src/scripts/npm_run.sh ${npm_command} -- ${jstestfuzz_vars} ${in_patch_build_flag} ${maybe_use_es_modules} --branch ${branch_name}
