#!/usr/bin/env bash
# shellcheck disable=SC1090
# shellcheck disable=SC1091
if [[ -z "${INCLUDED_ENV_INC_SH:-}" ]]; then
  . ci/utils.inc.sh
  . ci/env-common.inc.sh

  OS="$(get_os)"
  export OS

  . "ci/env-${OS}.inc.sh"

  : "${MAKE_PARALLEL:=$CORES}"
  export MAKE_PARALLEL

  . ci/lib/install_functions.inc.sh


  : "${MAKE_PARALLEL:=$CORES}"
  export MAKE_PARALLEL

  : "${CTEST_PARALLEL:=$CORES}"
  export CTEST_PARALLEL

  : "${PARALLEL_TEST_PROCESSORS:=$CORES}"
  export PARALLEL_TEST_PROCESSORS

  export INCLUDED_ENV_INC_SH=1
fi
