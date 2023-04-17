DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit

# Set the suite name to be the task name by default; unless overridden with the `suite` expansion.
suite_name=${task_name}
if [[ -n ${suite} ]]; then
  suite_name=${suite}
fi

timeout_factor=""
if [[ -n "${exec_timeout_factor}" ]]; then
  timeout_factor="--exec-timeout-factor ${exec_timeout_factor}"
fi

build_variant_for_timeout=${build_variant}
if [[ -n "${burn_in_bypass}" ]]; then
  # burn_in_tags may generate new build variants, if we are running on one of those build variants
  # we should use the build variant it is based on for determining the timeout. This is stored in
  # the `burn_in_bypass` expansion.
  build_variant_for_timeout=${burn_in_bypass}
fi

if [[ -n "${alias}" ]]; then
  evg_alias=${alias}
else
  evg_alias="evg-alias-absent"
fi

resmoke_test_flags=""
if [[ -n "${test_flags}" ]]; then
  resmoke_test_flags="--test-flags='${test_flags}'"
fi

activate_venv
PATH=$PATH:$HOME:/ eval $python buildscripts/evergreen_task_timeout.py \
  $timeout_factor \
  $resmoke_test_flags \
  --install-dir "${install_dir}" \
  --task-name ${task_name} \
  --suite-name ${suite_name} \
  --project ${project} \
  --build-variant $build_variant_for_timeout \
  --evg-alias $evg_alias \
  --timeout ${timeout_secs} \
  --exec-timeout ${exec_timeout_secs} \
  --evg-api-config ./.evergreen.yml \
  --evg-project-config ${evergreen_config_file_path} \
  --out-file task_timeout_expansions.yml
