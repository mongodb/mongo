DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -evo pipefail

cd src/jstestfuzz

# Store 'jstestfuzz_vars' into 'vars'. We will use 'vars' instead of 'jstestfuzz_vars' for
# the rest of this shell script.
vars="${jstestfuzz_vars}"

# If the "--jstestfuzzGitRev" option is present in 'vars', copy the option's value into
# 'jstestfuzz_gitrev' and remove it from 'vars', and then reset the jstestfuzz repo to the
# specified git revision.
echo "${vars}" | grep -q -- '--jstestfuzzGitRev[ \t]\+[A-Za-z0-9]\+' && RET=0 || RET=$?
if [ $RET -eq 0 ]; then
  jstestfuzz_gitrev=$(echo "${vars}" | sed -e 's/.*--jstestfuzzGitRev[ \t]\+\([A-Za-z0-9]\+\).*/\1/')
  vars=$(echo "${vars}" | sed -e 's/\(.*\)--jstestfuzzGitRev[ \t]\+[A-Za-z0-9]\+\(.*\)/\1\2/')

  for i in {1..5}; do
    git reset --hard "${jstestfuzz_gitrev}" && RET=0 && break || RET=$? && sleep 5
    echo "Failed to reset jstestfuzz to git revision ${jstestfuzz_gitrev}, retrying..."
  done

  if [ $RET -ne 0 ]; then
    echo "Failed to reset jstestfuzz to git revision ${jstestfuzz_gitrev}"
    exit $RET
  fi
fi

# If the "--metaSeed" option is present in 'vars', copy the option's value into 'meta_seed'
# and remove it from 'vars', and then generate a seed using 'meta_seed' and 'task_name' and
# pass this generated seed to "npm_run.sh" below.
generated_seed_flag=""
echo "${vars}" | grep -q -- '--metaSeed[ \t]\+[0-9]\+' && RET=0 || RET=$?
if [ $RET -eq 0 ]; then
  # Throw an error if the "--seed" option and the "--metaSeed" option are both present.
  echo "${vars}" | grep -q -- '--seed[ \t]\+[0-9]\+' && RET=0 || RET=$?
  if [ $RET -eq 0 ]; then
    echo "Cannot use the --seed option and the --metaSeed option together"
    exit 1
  fi

  # Store the meta seed value into 'meta_seed' and remove the "--metaSeed" option from 'vars'.
  meta_seed=$(echo "${vars}" | sed -e 's/.*--metaSeed[ \t]\+\([0-9]\+\).*/\1/')
  vars=$(echo "${vars}" | sed -e 's/\(.*\)--metaSeed[ \t]\+[0-9]\+\(.*\)/\1\2/')

  # If 'task_name' matches the pattern that we use for generated task names, extract the number
  # from 'task_name' and generate a seed using this number and 'meta_seed' together. Otherwise,
  # just use 'meta_seed' itself for the seed.
  echo "${task_name}" | grep -q '_[0-9]\+\(-.*\)\?$' && RET=0 || RET=$?
  if [ $RET -eq 0 ]; then
    task_num=$(echo "${task_name}" | sed -e 's/.*_\([0-9]\+\)\(-.*\)\?$/\1/')
    generated_seed=$((meta_seed + 971 * (task_num + 1)))
    generated_seed_flag="--seed ${generated_seed}"
  else
    generated_seed_flag="--seed ${meta_seed}"
  fi
fi

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

./src/scripts/npm_run.sh ${npm_command} -- ${vars} ${generated_seed_flag} ${in_patch_build_flag} ${maybe_use_es_modules} --branch ${branch_name}
